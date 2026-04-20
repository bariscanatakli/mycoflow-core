"use strict";
"require view";
"require rpc";
"require poll";
"require ui";
"require fs";

var callMycoPolicySet = rpc.declare({
  object: "myco",
  method: "policy_set",
  params: ["bandwidth_kbit"],
});

var callMycoBoost = rpc.declare({
  object: "myco",
  method: "policy_boost",
  params: ["step_kbit"],
});

var callMycoThrottle = rpc.declare({
  object: "myco",
  method: "policy_throttle",
  params: ["step_kbit"],
});

var callMycoPersonaAdd = rpc.declare({
  object: "myco",
  method: "persona_add",
  params: ["persona"],
});

var callMycoPersonaDelete = rpc.declare({
  object: "myco",
  method: "persona_delete",
});

function formatBps(bps) {
  if (bps >= 1e6) return (bps / 1e6).toFixed(2) + " Mbps";
  if (bps >= 1e3) return (bps / 1e3).toFixed(1) + " kbps";
  return bps.toFixed(0) + " bps";
}

// Service classes (Phase 6: per-flow classification).
// Colors follow CAKE tin ordering: warm = latency-sensitive, cool = bulk.
var SERVICE_STYLES = {
  game_rt:         { color: "#E91E63", icon: "🎯", label: "GAME" },
  voip_call:       { color: "#FF5722", icon: "📞", label: "VOIP" },
  video_conf:      { color: "#FF9800", icon: "🎥", label: "CONF" },
  video_live:      { color: "#FFC107", icon: "📡", label: "LIVE" },
  video_vod:       { color: "#CDDC39", icon: "📺", label: "VOD"  },
  web_interactive: { color: "#4CAF50", icon: "🌐", label: "WEB"  },
  bulk_dl:         { color: "#03A9F4", icon: "⬇️",  label: "BULK" },
  file_sync:       { color: "#00BCD4", icon: "☁️",  label: "SYNC" },
  torrent:         { color: "#9C27B0", icon: "🧲", label: "TOR"  },
  game_launcher:   { color: "#3F51B5", icon: "🚀", label: "GLNCH"},
  system:          { color: "#607D8B", icon: "⚙️",  label: "SYS"  },
  unknown:         { color: "#9E9E9E", icon: "❓", label: "?"    },
};

function serviceBadge(service, demoted) {
  var s = SERVICE_STYLES[service] || SERVICE_STYLES["unknown"];
  var label = s.icon + " " + s.label + (demoted ? " ↓" : "");
  return E("span", {
    style:
      "display:inline-block;padding:2px 8px;border-radius:10px;" +
      "background:" + s.color + ";color:#fff;font-weight:bold;font-size:11px;" +
      (demoted ? "outline:2px dashed #F44336;" : ""),
    title: demoted ? "Demoted by RTT auto-corrector" : "",
  }, label);
}

function personaBadge(persona) {
  var colors = {
    interactive: "#2196F3",
    bulk: "#FF9800",
    unknown: "#9E9E9E",
  };
  var icons = {
    interactive: "🎮",
    bulk: "📦",
    unknown: "❓",
  };
  var color = colors[persona] || colors["unknown"];
  var icon = icons[persona] || icons["unknown"];
  return E(
    "span",
    {
      style:
        "display:inline-block;padding:4px 12px;border-radius:12px;" +
        "background:" +
        color +
        ";color:#fff;font-weight:bold;font-size:14px",
    },
    icon + " " + (persona || "unknown").toUpperCase(),
  );
}

function metricCard(label, value, unit, color) {
  return E(
    "div",
    {
      style:
        "display:inline-block;width:160px;margin:8px;padding:16px;" +
        "border-radius:12px;background:#f5f5f5;text-align:center;" +
        "box-shadow:0 2px 4px rgba(0,0,0,0.1)",
    },
    [
      E("div", { style: "font-size:12px;color:#666;margin-bottom:4px" }, label),
      E(
        "div",
        { style: "font-size:24px;font-weight:bold;color:" + (color || "#333") },
        value,
      ),
      E("div", { style: "font-size:11px;color:#999" }, unit || ""),
    ],
  );
}

return view.extend({
  title: _("MycoFlow Dashboard"),

  handleBoost: function (ev) {
    return callMycoBoost(1000).then(function () {
      ui.addNotification(
        null,
        E("p", _("Bandwidth boosted +1000 kbit")),
        "info",
      );
    });
  },

  handleThrottle: function (ev) {
    return callMycoThrottle(1000).then(function () {
      ui.addNotification(
        null,
        E("p", _("Bandwidth throttled -1000 kbit")),
        "info",
      );
    });
  },

  handleOverrideInteractive: function (ev) {
    return callMycoPersonaAdd("interactive").then(function () {
      ui.addNotification(
        null,
        E("p", _("Persona override: interactive")),
        "info",
      );
    });
  },

  handleOverrideBulk: function (ev) {
    return callMycoPersonaAdd("bulk").then(function () {
      ui.addNotification(null, E("p", _("Persona override: bulk")), "info");
    });
  },

  handleClearOverride: function (ev) {
    return callMycoPersonaDelete().then(function () {
      ui.addNotification(null, E("p", _("Persona override cleared")), "info");
    });
  },

  load: function () {
    return fs.read_direct("/tmp/myco_state.json").then(function(res) {
      try { return [JSON.parse(res), {}]; } catch(e) { return [{}, {}]; }
    }).catch(function() {
      return fs.read("/tmp/myco_state.json").then(function(res) {
         try { return [JSON.parse(res), {}]; } catch(e) { return [{}, {}]; }
      }).catch(function() { return [{}, {}]; });
    });
  },

  render: function (data) {
    var status = data[0] || {};
    var persona = data[1] || {};
    var metrics = status.metrics || {};
    var baseline = status.baseline || {};
    var policy = status.policy || {};

    var statusNode = E("div", { id: "mycoflow-status" });
    var logNode = E("pre", {
        id: "mycoflow-logs",
        style: "background:#333;color:#eee;padding:12px;border-radius:4px;overflow-y:auto;max-height:300px;font-family:monospace;font-size:12px;margin-bottom:16px"
    });
    var self = this;

    var updateStatus = function () {
      return Promise.all([
        fs.read_direct("/tmp/myco_state.json").catch(function() {
          return fs.read("/tmp/myco_state.json");
        }),
        fs.exec("/sbin/logread", ["-e", "myco", "-l", "30"]).then(function(res) {
          return res.stdout || res;
        }).catch(function() { return ""; })
      ]).then(function(results) {
        var res = results[0];
        var logData = results[1] || "No logs available.";
        logNode.textContent = logData;
        logNode.scrollTop = logNode.scrollHeight;

        var s = {};
        try { s = JSON.parse(res); } catch(e) {}
        
        var m = s.metrics || {};
        var b = s.baseline || {};
        var p = s.policy || {};

        var safeMode = s.safe_mode
          ? E(
              "div",
              {
                style:
                  "background:#f44336;color:#fff;padding:8px 16px;border-radius:8px;margin-bottom:16px;font-weight:bold",
              },
              "⚠️ SAFE MODE ACTIVE",
            )
          : "";

        var overrideInfo = s.persona_override
          ? E(
              "div",
              {
                style:
                  "background:#FF9800;color:#fff;padding:6px 12px;border-radius:8px;margin-bottom:12px",
              },
              "🔒 Override: " + (s.persona_override_value || "unknown"),
            )
          : "";

        statusNode.innerHTML = "";
        statusNode.appendChild(
          E("div", {}, [
            safeMode,
            overrideInfo,

            E("h3", { style: "margin-top:0" }, "📊 Live Metrics"),
            E("div", { style: "display:flex;flex-wrap:wrap;gap:4px" }, [
              metricCard(
                "RTT",
                m.rtt_ms ? m.rtt_ms.toFixed(1) : "—",
                "ms",
                m.rtt_ms > 50 ? "#f44336" : "#4CAF50",
              ),
              metricCard(
                "Jitter",
                m.jitter_ms ? m.jitter_ms.toFixed(1) : "—",
                "ms",
                m.jitter_ms > 10 ? "#FF9800" : "#4CAF50",
              ),
              metricCard("TX", formatBps(m.tx_bps || 0), "", "#2196F3"),
              metricCard("RX", formatBps(m.rx_bps || 0), "", "#9C27B0"),
              metricCard(
                "CPU",
                m.cpu_pct ? m.cpu_pct.toFixed(1) : "—",
                "%",
                m.cpu_pct > 40 ? "#f44336" : "#4CAF50",
              ),
            ]),

            E("h3", {}, "🎭 Persona"),
            E("div", { style: "margin:8px 0" }, [
              personaBadge(s.persona),
              E(
                "span",
                { style: "margin-left:16px;color:#666" },
                "Reason: " + (s.reason || "—"),
              ),
            ]),

            E("h3", {}, "📡 Policy"),
            E("div", { style: "margin:8px 0" }, [
              metricCard("Bandwidth", p.bandwidth_kbit || 0, "kbit", "#673AB7"),
              metricCard(
                "Baseline RTT",
                b.rtt_ms ? b.rtt_ms.toFixed(1) : "—",
                "ms",
                "#607D8B",
              ),
            ]),

            E("h3", { style: "margin-top:24px" }, "🌊 Active Flows (per-service)"),
            (s.flows && s.flows.length > 0) ? E("table", { class: "table cbi-section-table" }, [
              E("tr", { class: "tr table-titles" }, [
                E("th", { class: "th" }, "Service"),
                E("th", { class: "th" }, "Source"),
                E("th", { class: "th" }, "Destination"),
                E("th", { class: "th", style: "text-align:right" }, "Port"),
                E("th", { class: "th", style: "text-align:right" }, "Proto"),
                E("th", { class: "th", style: "text-align:right" }, "Mark"),
                E("th", { class: "th", style: "text-align:right" }, "RTT (ms)"),
                E("th", { class: "th" }, "State"),
              ])
            ].concat(s.flows.map(function(flow) {
              var state;
              if (flow.demoted) {
                state = E("span", { style: "color:#F44336;font-weight:bold" }, "demoted");
              } else if (flow.stable) {
                state = E("span", { style: "color:#4CAF50" }, "stable");
              } else {
                state = E("span", { style: "color:#9E9E9E" }, "probing");
              }
              return E("tr", { class: "tr cbi-rowstyle-1" }, [
                E("td", { class: "td" }, serviceBadge(flow.service, flow.demoted)),
                E("td", { class: "td" }, flow.src),
                E("td", { class: "td" }, flow.dst),
                E("td", { class: "td", style: "text-align:right" }, flow.dport),
                E("td", { class: "td", style: "text-align:right" }, flow.proto === 6 ? "TCP" : (flow.proto === 17 ? "UDP" : flow.proto)),
                E("td", { class: "td", style: "text-align:right" }, flow.mark),
                E("td", { class: "td", style: "text-align:right" }, flow.rtt_ms || "—"),
                E("td", { class: "td" }, state),
              ]);
            }))) : E("em", { style: "color:#999" }, "No classified flows (flow-aware mode may be disabled)."),

            E("h3", { style: "margin-top:24px" }, "📱 Connected Devices"),
            (s.devices && s.devices.length > 0) ? E("table", { class: "table cbi-section-table" }, [
              E("tr", { class: "tr table-titles" }, [
                E("th", { class: "th" }, "IP Address"),
                E("th", { class: "th" }, "Persona"),
                E("th", { class: "th", style: "text-align:right" }, "Flows"),
                E("th", { class: "th", style: "text-align:right" }, "Download"),
                E("th", { class: "th", style: "text-align:right" }, "Upload")
              ])
            ].concat(s.devices.map(function(d) {
              return E("tr", { class: "tr cbi-rowstyle-1" }, [
                E("td", { class: "td" }, d.ip),
                E("td", { class: "td" }, d.override ? [personaBadge(d.persona), " 🔒"] : personaBadge(d.persona)),
                E("td", { class: "td", style: "text-align:right" }, d.flows),
                E("td", { class: "td", style: "text-align:right" }, formatBps(d.rx_bps || 0)),
                E("td", { class: "td", style: "text-align:right" }, formatBps(d.tx_bps || 0))
              ]);
            }))) : E("em", {}, "No active devices tracked."),
          ]),
        );
      });
    };

    poll.add(updateStatus, 2);
    updateStatus();

    return E("div", { class: "cbi-map" }, [
      E("h2", {}, "🍄 MycoFlow — Bio-Inspired QoS"),
      statusNode,
      E("h3", { style: "margin-top:24px" }, "📝 System Logs"),
      logNode,
      E("hr"),
      E("h3", {}, "🎛️ Manual Controls"),
      E("div", { style: "display:flex;gap:8px;flex-wrap:wrap;margin:12px 0" }, [
        E(
          "button",
          {
            class: "cbi-button cbi-button-positive",
            click: ui.createHandlerFn(self, "handleBoost"),
          },
          "⬆ Boost",
        ),
        E(
          "button",
          {
            class: "cbi-button cbi-button-negative",
            click: ui.createHandlerFn(self, "handleThrottle"),
          },
          "⬇ Throttle",
        ),
      ]),
      E("h4", {}, "Persona Override"),
      E("div", { style: "display:flex;gap:8px;flex-wrap:wrap;margin:12px 0" }, [
        E(
          "button",
          {
            class: "cbi-button cbi-button-action",
            click: ui.createHandlerFn(self, "handleOverrideInteractive"),
          },
          "🎮 Interactive",
        ),
        E(
          "button",
          {
            class: "cbi-button cbi-button-action",
            click: ui.createHandlerFn(self, "handleOverrideBulk"),
          },
          "📦 Bulk",
        ),
        E(
          "button",
          {
            class: "cbi-button cbi-button-reset",
            click: ui.createHandlerFn(self, "handleClearOverride"),
          },
          "🔄 Clear Override",
        ),
      ]),
    ]);
  },

  handleSaveApply: null,
  handleSave: null,
  handleReset: null,
});
