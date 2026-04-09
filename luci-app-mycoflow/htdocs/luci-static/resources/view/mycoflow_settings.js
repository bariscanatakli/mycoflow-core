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

    return m.render();
  }
});
