// The HaLow radio form, shared by the setup and settings pages. It offers the same choices as
// OpenMANET's own HaLow channel settings (LuCI on OpenMANET 1.8.0): a width, then a channel of
// that width shown as "28 (916 MHz)", then the primary channel width (auto, 2 MHz, 1 MHz) and
// the primary 1 MHz index (auto, or the indices whose primary channel is a channel of the
// country). The channel table comes from the meshpoint (/api/radio).
//
// Elements it fills, when the page has them: r_width, r_chan, r_pbw, r_pidx (selects),
// r_beacon (number), r_tx (select), r_ap (select), r_info (text).
(function () {
  const $ = (id) => document.getElementById(id);
  let data = null;

  function setOptions(sel, options, keep) {
    if (!sel) return;
    const old = keep !== undefined ? String(keep) : sel.value;
    sel.textContent = "";
    for (const [value, label] of options) {
      const o = document.createElement("option");
      o.value = String(value);
      o.textContent = label;
      sel.appendChild(o);
    }
    if ([...sel.options].some((o) => o.value === old)) sel.value = old;
  }

  const fmtMhz = (mhz) => String(Math.round(mhz * 1000) / 1000);
  const chanInfo = (chan) => data && data.channels.find((c) => c.chan === Number(chan));

  // LuCI's allowedS1gChanIndex: the primary channel's centre must be a channel centre in the table.
  function allowedIndex(ch, primWidth, idx) {
    if (ch.bw === 1) return true;
    let offset;
    if (primWidth === "2") offset = -ch.bw / 2 + 1 + Math.floor(idx / 2) * 2;
    else if (primWidth === "1") offset = -ch.bw / 2 + 0.5 + idx;
    else return false;
    const want = Math.round(1000 * (ch.mhz + offset));
    return data.channels.some((c) => Math.round(c.mhz * 1000) === want);
  }

  function fillChannels(keep) {
    const bw = Number($("r_width").value);
    const list = data.channels.filter((c) => c.bw === bw);
    const opts = list.map((c) => [c.chan, c.chan + " (" + fmtMhz(c.mhz) + " MHz)"]);
    // Like LuCI: a channel that isn't in this width's list falls back to the middle one.
    let want = keep !== undefined ? String(keep) : $("r_chan").value;
    if (!list.some((c) => String(c.chan) === want) && list.length) want = list[Math.floor(list.length / 2)].chan;
    setOptions($("r_chan"), opts, want);
    fillPrimary();
  }

  function fillPrimary(keepWidth, keepIdx) {
    const ch = chanInfo($("r_chan").value);
    if (!ch) return;
    setOptions($("r_pbw"), ch.bw === 1 ? [["auto", "auto"], ["1", "1 MHz"]]
                                      : [["auto", "auto"], ["2", "2 MHz"], ["1", "1 MHz"]], keepWidth);
    if (![...$("r_pbw").options].some((o) => o.value === $("r_pbw").value)) $("r_pbw").value = "auto";
    fillIndex(keepIdx);
  }

  function fillIndex(keep) {
    const ch = chanInfo($("r_chan").value);
    if (!ch) return;
    const opts = [["auto", "auto"]];
    for (let i = 0; i < ch.bw; i++) if (allowedIndex(ch, $("r_pbw").value, i)) opts.push([i, String(i)]);
    setOptions($("r_pidx"), opts, keep);
    if (![...$("r_pidx").options].some((o) => o.value === $("r_pidx").value)) $("r_pidx").value = "auto";
    describe();
  }

  // What auto works out to, as OpenMANET does it: 2 MHz primary on 4 and 8 MHz channels,
  // index (width - 1) / 2.
  function describe() {
    const ch = chanInfo($("r_chan").value);
    if (!ch || !$("r_info")) return;
    const pbw = $("r_pbw").value === "auto" ? (ch.bw === 4 || ch.bw === 8 ? 2 : 1) : Number($("r_pbw").value);
    const pidx = $("r_pidx").value === "auto" ? Math.floor((ch.bw - 1) / 2) : Number($("r_pidx").value);
    $("r_info").textContent = fmtMhz(ch.mhz) + " MHz, " + ch.bw + " MHz wide, operating class " + ch.op +
      "; primary " + pbw + " MHz at index " + pidx + ". Country " + data.country + ".";
  }

  async function load() {
    const r = await fetch("/api/radio", { cache: "no-store" });
    if (!r.ok) throw Object.assign(new Error("radio " + r.status), { status: r.status });
    data = await r.json();
    const s = data.saved;
    const widths = [...new Set(data.channels.map((c) => c.bw))].sort((a, b) => b - a);
    const cur = chanInfo(s.chan);
    setOptions($("r_width"), widths.map((w) => [w, w + " MHz"]), cur ? cur.bw : widths[0]);
    fillChannels(s.chan);
    fillPrimary(s.prim_bw === 0 ? "auto" : s.prim_bw, s.prim_idx < 0 ? "auto" : s.prim_idx);
    if ($("r_beacon")) $("r_beacon").value = s.beacon;
    const tx = [[0, "driver default (regulatory maximum)"]];
    for (let d = 1; d <= 30; d++) tx.push([d, d + " dBm (" + Math.round(Math.pow(10, d / 10)) + " mW)"]);
    setOptions($("r_tx"), tx, s.tx_dbm);
    const ap = [];
    for (let c = 1; c <= data.ap_max_chan; c++) ap.push([c, c + " (" + (2407 + 5 * c) + " MHz)"]);
    setOptions($("r_ap"), ap, s.ap_chan);
    return data;
  }

  function values() {
    const s = data.saved;
    return {
      chan: Number($("r_chan").value),
      prim_bw: $("r_pbw").value === "auto" ? 0 : Number($("r_pbw").value),
      prim_idx: $("r_pidx").value === "auto" ? -1 : Number($("r_pidx").value),
      beacon: $("r_beacon") ? Number($("r_beacon").value) : s.beacon,
      tx_dbm: $("r_tx") ? Number($("r_tx").value) : s.tx_dbm,
      ap_chan: $("r_ap") ? Number($("r_ap").value) : s.ap_chan,
    };
  }

  function wire() {
    if (!$("r_width")) return;
    $("r_width").addEventListener("change", () => fillChannels());
    $("r_chan").addEventListener("change", () => fillPrimary());
    $("r_pbw").addEventListener("change", () => fillIndex());
    $("r_pidx").addEventListener("change", describe);
  }

  window.Radio = { load, values, loaded: () => data !== null };
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", wire);
  else wire();
})();
