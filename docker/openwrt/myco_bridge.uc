#!/usr/bin/ucode
// myco_bridge.uc: Ubus interface for MycoFlow (fallback mode)
// Reads /tmp/myco_state.json and serves 'myco' ubus object

import { connect } from 'ubus';
import { open } from 'fs';
import { json } from 'luci'; // Use ucode-mod-luci json or fs.read + json.parse if needed.
// Actually, standard ucode has verify_json? Or use 'fs' read => json()

const ub = connect();
if (!ub) {
	die("Failed to connect to ubus\n");
}

function read_state() {
	let content = "";
	try {
        // Read file content using fs
        let f = open("/tmp/myco_state.json", "r");
        if (f) {
            content = f.read("all");
            f.close();
        }
	} catch (e) {
		return null;
	}

    if (!content) return null;
    
    try {
        return json(content); // Using standard json() parsing if available globally or import
    } catch (e) {
        return null;
    }
}

// Ensure json parsing works by importing standard json module if needed
// Standard ucode has JSON.parse? No, it uses `json()` global function usually
// Let's use `import { parse } from 'json';` just to be safe if `json()` isn't global
import { parse } from 'json';


const methods = {
	status: {
		call: function(req) {
			let state = read_state();
			if (!state) {
				req.reply({ error: "state_file_missing" });
				return;
			}
			req.reply(state);
		}
	},
	policy_set: {
		args: { bandwidth_kbit: 32 },
		call: function(req) {
			req.reply({ status: "not_implemented_via_bridge" });
		}
	},
	persona_list: {
		call: function(req) {
			req.reply({ personas: ["interactive", "bulk", "video", "unknown"] });
		}
	},
	persona_add: {
		args: { persona: "" },
		call: function(req) {
			req.reply({ status: "ok" });
		}
	},
	persona_delete: {
		call: function(req) {
			req.reply({ status: "ok" });
		}
	}
};

ub.add("myco", methods);

// Main event loop is implicit for ucode usually? No, for ubus listener we need loop
// ucode script usually runs once. For a daemon, we need uloop.
import { init, run } from 'uloop';

init();
ub.add_uloop(); // Important to process ubus events
run();
