"use strict";
"require form";
"require view";

return view.extend({
  render: function () {
    var m, s, o;

    m = new form.Map(
      "mycoflow",
      _("MycoFlow Settings"),
      _("Configure bandwidth limits, performance, and general behavior of the MycoFlow QoS daemon.")
    );

    s = m.section(form.NamedSection, "core", "mycoflow", _("General Settings"));
    s.anonymous = true;
    s.addremove = false;

    o = s.option(form.Flag, "enabled", _("Enabled"));
    o.rmempty = false;

    o = s.option(form.Value, "egress_iface", _("Egress Interface"), _("WAN interface to shape traffic on (e.g. eth0, wan, br-lan)"));
    o.placeholder = "br-lan";
    o.rmempty = false;

    o = s.option(form.Flag, "per_device", _("Enable Per-Device Persona Tracking"),
      _("Track individual device metrics and assign them their own personas."));
    o.rmempty = false;

    o = s.option(form.Flag, "no_tc", _("Disable Traffic Control (Dry Run)"), 
      _("Run in dry-run mode without modifying iptables or CAKE disciplines."));
    o.rmempty = false;

    // Bandwidth
    s = m.section(form.NamedSection, "core", "mycoflow", _("Bandwidth & Policy"));
    s.anonymous = true;
    s.addremove = false;

    o = s.option(form.Value, "max_bandwidth_kbit", _("Max Bandwidth (kbit)"), _("Absolute upper limit of network bandwidth."));
    o.datatype = "uinteger";
    
    o = s.option(form.Value, "min_bandwidth_kbit", _("Min Bandwidth (kbit)"), _("Absolute lower limit of network bandwidth."));
    o.datatype = "uinteger";

    o = s.option(form.Value, "bandwidth_step_kbit", _("Bandwidth Step (kbit)"), _("Amount of bandwidth change per cycle."));
    o.datatype = "uinteger";

    o = s.option(form.Flag, "ingress_enabled", _("Enable Download QoS (Ingress)"), _("Enable traffic shaping on incoming download traffic via IFB."));
    o.rmempty = false;

    o = s.option(form.Value, "ingress_iface", _("Ingress Interface"), _("Virtual IFB interface used to mirror download traffic."));
    o.depends("ingress_enabled", "1");
    o.placeholder = "ifb0";
    o.rmempty = false;

    o = s.option(form.Value, "ingress_bandwidth_kbit", _("Max Download Bandwidth (kbit)"), _("Upper limit for download speed. Leave 0 or empty to dynamically match upload speed."));
    o.depends("ingress_enabled", "1");
    o.datatype = "uinteger";

    // Tuning & Advanced
    s = m.section(form.NamedSection, "core", "mycoflow", _("Tuning & Advanced"));
    s.anonymous = true;
    s.addremove = false;

    o = s.option(form.Value, "sample_hz", _("Sample Frequency (Hz)"));
    o.datatype = "uinteger";

    o = s.option(form.Value, "probe_host", _("Ping/Probe Host"), _("IP address to ping for latency metrics (e.g. 8.8.8.8)"));
    o.placeholder = "1.1.1.1";
    o.datatype = "host";

    o = s.option(form.Flag, "dummy_metrics", _("Use Dummy Metrics"), _("Simulate RTT and Jitter instead of actual ping (for testing)"));
    o.rmempty = false;

    o = s.option(form.Value, "baseline_samples", _("Baseline Sample Count"));
    o.datatype = "uinteger";

    o = s.option(form.Value, "ewma_alpha", _("EWMA Alpha Smoothing"), _("Alpha value for Exponential Weighted Moving Average (0.0 to 1.0)"));
    o.datatype = "float";

    o = s.option(form.Value, "max_cpu", _("CPU Usage Limit (%)"));
    o.datatype = "uinteger";

    o = s.option(form.Flag, "ebpf_enabled", _("Enable eBPF Hardware Acceleration"));
    
    o = s.option(form.Value, "ebpf_obj", _("eBPF Object Path"));
    o.depends("ebpf_enabled", "1");

    // Device Overrides
    s = m.section(form.TypedSection, "device", _("Device Overrides"), _("Assign fixed personas to specific devices by IP address."));
    s.addremove = true;
    s.anonymous = true;

    o = s.option(form.Value, "ip", _("IP Address"));
    o.datatype = "ip4addr";
    o.placeholder = "192.168.1.x";
    o.rmempty = false;

    o = s.option(form.Value, "mac", _("MAC Address (Optional)"));
    o.datatype = "macaddr";
    o.placeholder = "00:11:22:33:44:55";

    o = s.option(form.ListValue, "persona", _("Persona"));
    o.value("unknown", _("Unknown (Auto)"));
    o.value("gaming", _("Gaming / Interactive"));
    o.value("video", _("Video Call"));
    o.value("streaming", _("Streaming"));
    o.value("bulk", _("Bulk / Download"));
    o.value("torrent", _("Torrent / P2P"));
    o.value("voip", _("VoIP"));
    o.rmempty = false;

    return m.render();
  }
});
