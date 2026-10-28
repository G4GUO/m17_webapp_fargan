#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include "m17defines.h"
#include "audio.h"
#include "bluetooth.h"
#include "fargan.h"
#include "recorder.h"
#ifdef HAVE_SIM
#include "radio_sim.h"
#endif

void help(void){
	gui_help();
}
//
// Pad a Null terminated address into a 9 character address
//
void pad_address(const char *ain, char *aout){
	// Always fill the whole 9 characters and always terminate - the address is
	// fixed width on air, space padded.  The original padded only when the name
	// was LONGER than 9 (exactly backwards, the braces on the if were missing)
	// and never wrote a terminator at all, so a short callsign left four
	// uninitialised bytes of stack in the string.  That is what turned "G4GUO"
	// into "G4GUORADI": the tail came from the "mode radio" line parsed moments
	// earlier ("G4GUOGATE" when the mode was gate).  The bad address was then
	// encoded and transmitted, not just displayed.
	int n = (int)strlen(ain);
	if( n > 9 ) n = 9;
	memset(aout,' ',9);
	for( int i = 0; i < n; i++){
		aout[i] = toupper(ain[i]);
	}
	aout[9] = 0;
}
// Keep the chosen mode for the next start.  config.txt holds the same commands
// the console takes, one per line, so its "mode" line is replaced in place.
// Writing a temporary file and renaming it means a failure part way through
// cannot leave a half written config behind.
static void save_line_to_config(const char *key, const char *line){
	char buf[256];
	size_t klen = strlen(key);
	const char *tmp = "config.txt.tmp";
	FILE *in  = fopen("config.txt","r");
	FILE *out = fopen(tmp,"w");
	bool wrote = false;
	if(out == NULL){
		printf("m17web: cannot write config.txt, the mode will not be kept\n");
		return;
	}
	if(in != NULL){
		while(fgets(buf, sizeof(buf), in) != NULL){
			char *p = buf;
			while(*p == ' ' || *p == '	') p++;
			if(strncmp(p, key, klen) == 0 &&
			   (p[klen] == ' ' || p[klen] == '	' || p[klen] == '\n')){
				if(wrote == false){
					fprintf(out, "%s\n", line);
					wrote = true;
				}
				continue;                       // the old line is dropped
			}
			fputs(buf, out);
		}
		fclose(in);
	}
	if(wrote == false) fprintf(out, "%s\n", line);        // not there: add it
	fclose(out);
	if(rename(tmp, "config.txt") != 0)
		printf("m17web: could not replace config.txt\n");
	else
		printf("m17web: %s kept for the next start\n", line);
}

static void save_mode_to_config(const char *mode){
	char line[64];
	snprintf(line, sizeof(line), "mode %s", mode);
	save_line_to_config("mode", line);
}

// Put the routes back in step with the Bluetooth selections: a direction that
// has a device chosen moves to Bluetooth, one without goes back to the sound
// card.  Per direction, so choosing a speaker does not disturb a microphone
// that is working - and the choice is kept in config.txt as the route lines the
// interface writes.
static void bt_sync_routes(void){
	AudioRoute out = bt_out_mac()[0] ? AROUTE_BT : AROUTE_CARD;
	AudioRoute in  = bt_in_mac()[0]  ? AROUTE_BT : AROUTE_CARD;
	audio_apply_routes(out, in);
	char line[48];
	snprintf(line, sizeof(line), "spk %s", audio_route_name(out));
	save_line_to_config("spk", line);
	snprintf(line, sizeof(line), "mic %s", audio_route_name(in));
	save_line_to_config("mic", line);
}

void mmi_parse(char *cmd){
	int nt = 0;
	bool valid = false;
	char *token[10];

	// Drop trailing line endings and whitespace: the tokeniser only splits on
	// spaces, so "radio\n" from config.txt used to become a bad token value.
	int cl = (int)strlen(cmd);
	while(cl > 0 && (cmd[cl-1] == '\n' || cmd[cl-1] == '\r' ||
	                 cmd[cl-1] == ' '  || cmd[cl-1] == '	'))
		cmd[--cl] = 0;
	if(cl == 0) return;

	token[nt] = strtok(cmd," ");
	while(token[nt] != NULL){
		if( ++nt == 10 ) break;
		token[nt] = strtok(NULL," ");
	}

	// Start of commands

	// Comment
	if(strncmp(token[0],"#",1)==0){
		valid = true;;
	}

	// Transmitter Gain
	if(strncmp(token[0],"tg",2)==0){
		if(	nt == 2 ){
			float gain = atof(token[1]);
			if((gain <= 1.0)&&(gain>=0)){
				radio_set_tx_gain(gain);
				valid = true;
			}
		}
	}

	// Receiver Gain
	if(strncmp(token[0],"rg",2)==0){
		if(	nt == 2 ){
			float gain = atof(token[1]);
			if((gain <= 1.0)&&(gain>=0)){
				radio_set_rx_gain(gain);
				valid = true;
			}
		}
	}

	// Transmitter frequency
	if(strncmp(token[0],"tf",2)==0){
		if(	nt == 2 ){
			uint64_t freq = atoll(token[1]);
			radio_set_tx_frequency(freq);
			m17_db_set_tx_freq(freq);
			valid = true;
		}
	}

	// Receiver frequency
	if(strncmp(token[0],"rf",2)==0){
		if(	nt == 2 ){
			uint64_t freq = atoll(token[1]);
			radio_set_rx_frequency(freq);
			m17_db_set_rx_freq(freq);
			valid = true;
		}
	}

	// Frequency correction factor
	if(strncmp(token[0],"fc",2)==0){
		if(	nt == 2 ){
			double factor = atof(token[1]);
			radio_set_freq_correction_factor(factor);
			valid = true;
		}
	}

	// Automatic Frequency Control
	if(strncmp(token[0],"afc",3)==0){
		if(	nt == 2 ){
			if(strncmp(token[1],"on",2)==0){
				radio_set_afc_on();
				valid = true;
			}else{
				if(strncmp(token[1],"off",3)==0){
					radio_set_afc_off();
					valid = true;
				}
			}
		}
	}

	// Quit
	if(strncmp(token[0],"q",1)==0){
		m17_txrx_disable();
	}

	// Transmit carrier
	if(strncmp(token[0],"tc",2)==0){
		m17_txrx_ptt_carrier();
		valid = true;
	}

	// Transmit
	if(strncmp(token[0],"tx",2)==0){
		m17_txrx_ptt_on();
		valid = true;
	}

	// Duplex
	if(strncmp(token[0],"td",2)==0){
		m17_txrx_ptt_duplex();
		valid = true;
	}

	// Receive
	if(strncmp(token[0],"rx",2)==0){
		m17_txrx_ptt_off();
		valid = true;
	}

	// Source Address
	if(strncmp(token[0],"sa",2)==0){
		char text[16];
		pad_address(token[1], text);
		m17_txrx_set_src_add(text);
		valid = true;
	}

	// Gateway address
	if(strncmp(token[0],"ga",2)==0){
		char text[16];
		pad_address(token[1], text);
		char c = token[2][0];
		text[8] = c;
		m17_txrx_set_gate_add(text);
		valid = true;
	}
	// Destination Address.  A leading "#" is dropped: it is not one of the 40 M17
	// characters (space, A-Z, 0-9, - / .), so "#PARROT" cannot be encoded - and
	// "PARROT" is what the reflector actually looks for.
	if(strncmp(token[0],"da",2)==0){
		char text[16];
		const char *want = (token[1][0] == '#') ? token[1] + 1 : token[1];
		pad_address(want, text);
		m17_txrx_set_dest_add(text);
		valid = true;
	}
	// Broadcast destination Address
	if(strncmp(token[0],"ba",2)==0){
		m17_txrx_set_brd_add();
		valid = true;
	}

	// Position log: csv on|off|status, or the interval in seconds
	if(strncmp(token[0],"csv",3)==0){
		if(nt == 1 || strncmp(token[1],"st",2)==0){
			printf("CSV position log is %s, every %d s, %d rows\n",
			       gps_csv_on() ? "on" : "off", gps_csv_interval(), gps_csv_rows());
			printf("  file %s\n\r", gps_csv_path());
		} else if(strncmp(token[1],"on",2)==0 || strncmp(token[1],"off",3)==0){
			bool on = (strncmp(token[1],"on",2)==0);
			gps_csv_set(on, nt > 2 ? atoi(token[2]) : 0);
			printf("CSV position log %s%s\n\r", on ? "on, every " : "off",
			       on ? "" : "");
			if(on) printf("CSV position log file %s\n\r", gps_csv_path());
		} else {
			gps_csv_set(gps_csv_on(), atoi(token[1]));      // just a new interval
			printf("CSV position log every %d s\n\r", gps_csv_interval());
		}
		valid = true;
	}

	// GPS receiver: status, or pick a different device / speed
	if(strncmp(token[0],"gps",3)==0){
		if(nt == 1){
			char st[160], grid[12];
			gps_status_text(st, sizeof(st));
			gps_maidenhead(grid, sizeof(grid));
			printf("GPS %s at %d baud\n", gps_device()[0] ? gps_device() : "(auto-detect)", gps_baud());
			printf("  status   %s\n", gps_status());
			printf("  position %s  locator %s\n", st, grid);
			printf("  altitude %d m\n", (int)m_gps.alt);
			printf("  position in META %s\n", m17_tx_gnss_enabled()
			       ? (m17_tx_gnss() ? "on, announced" : "on, waiting for a fix")
			       : "off");
			printf("  CSV log %s (%d rows in %s)\n\r",
			       gps_csv_on() ? "on" : "off", gps_csv_rows(), gps_csv_path());
		} else if(strcmp(token[1],"meta") == 0){
			// Whether our position goes out in the META field.  Off means the
			// receiver still runs (locator, compass, CSV) but nothing is announced.
			bool on = !(nt > 2 && (strcasecmp(token[2],"off") == 0 || strcmp(token[2],"0") == 0));
			if(nt == 2){
				printf("position in META is %s\n\r", m17_tx_gnss_enabled() ? "on" : "off");
			} else {
				m17_tx_set_gnss_enabled(on);
				save_line_to_config("gps meta", on ? "gps meta on" : "gps meta off");
				printf("position in META %s\n\r", on ? "on" : "off");
			}
		} else {
			gps_close();
			gps_set_device(token[1], nt > 2 ? atoi(token[2]) : 0);
			gps_open();
			printf("GPS reopened on %s\n\r", token[1]);
		}
		valid = true;
	}

	// Blank frames sent before the voice, in codec frames (2 = 40 ms)
	if(strncmp(token[0],"lead",4)==0){
		if(nt == 2){
			m17_tx_set_lead_frames(atoi(token[1]));
			valid = true;
		}
	}

	//   radio  - RF in, local audio out (the normal voice mode)
	//   loop   - the audio is looped back inside the program, for practice
	//   gate   - the radio is bridged to a reflector
	//   client - local audio is bridged to a reflector
	// A mode may be changed while running: the thread for the old mode stands
	// down (see m17_txrx_mode_changed) and the dispatcher starts the new one.
	// The choice is written to config.txt so it survives a restart.
	if(strncmp(token[0],"mode",4) == 0 ){
		CircuitType want = m17_db_get_chan_type();
		bool known = true;
		if(strncmp(token[1],"loop",4) == 0)        want = ASTOAS;
		else if(strncmp(token[1],"radio",5) == 0)  want = DRTOAS;
		else if(strncmp(token[1],"gate",4) == 0)   want = DRTODN;
		else if(strncmp(token[1],"client",6) == 0) want = ASTODN;
		else known = false;

		if(!known){
			gui_cmd_resp("mode: radio, loop, gate or client");
			valid = true;
		} else {
			bool was_net = (m17_db_get_chan_type() == DRTODN || m17_db_get_chan_type() == ASTODN);
			bool is_net  = (want == DRTODN || want == ASTODN);
			gui_save_mode(token[1]);

			if(want != m17_db_get_chan_type()){
				if(was_net && !is_net){
					// Leaving a network mode: leave the reflector politely first
					m17_net_disconnect_from_reflector();
					m17_net_close();
				}
				m17_db_set_chan_type(want);
				m17_txrx_mode_changed();
				save_mode_to_config(token[1]);
			}
			if(is_net) m17_net_open();      // needed at startup and when entering

			char text[80];
			snprintf(text, sizeof(text), "mode %s", token[1]);
			printf("m17web: %s\n", text);
			fflush(stdout);
			gui_cmd_resp(text);
			valid = true;
		}
	}

	if(strncmp(token[0],"w",1)==0){
		gui_update_clear();
		valid = true;
	}

	if(strncmp(token[0],"conn",4)==0){
		if(nt == 3){
			char c = token[2][0];
			// The UDP sockets used to be opened only by the gate/client modes,
			// so "connect" in the normal radio mode sent nothing at all.
			m17_net_open();
            m17_net_connect_to_reflector( token[1], m17_db_get_src(), c);
		    valid = true;
		}
	}
	if(strncmp(token[0],"disc",4)==0){
		m17_net_disconnect_from_reflector();
		valid = true;
	}

	// Audio routing: alsa (Pi sound card, direct), pulse (Pi sound card via
	// PulseAudio), browser (web UI), file, both, none.
	// "audio alsa plughw:3,0" also selects the ALSA device.
	//
	// The interface does not use any of those any more: it selects the speaker
	// and the microphone separately, with "audio apply spk <route> mic <route>"
	// (see below).
	if(strncmp(token[0],"audio",5)==0){
		if(nt == 1){
			char line[160];
			double mg = audio_get_mic_gain(), sv = audio_get_spk_vol();
			snprintf(line, sizeof(line), "speaker %s, microphone %s",
			         audio_route_name(audio_get_route(true)),
			         audio_route_name(audio_get_route(false)));
			gui_cmd_resp(line);
			snprintf(line, sizeof(line), "mic gain %d%% (%+.1f dB), speaker volume %d%% (%+.1f dB)",
			         (int)(mg * 100.0 + 0.5), 20.0 * log10(mg > 0.001 ? mg : 0.001),
			         (int)(sv * 100.0 + 0.5), 20.0 * log10(sv > 0.001 ? sv : 0.001));
			gui_cmd_resp(line);
			snprintf(line, sizeof(line), "alsa %s%s%s", audio_local_device()[0] ? audio_local_device() : "(none)",
			         audio_backend_error()[0] ? " - " : "", audio_backend_error());
			gui_cmd_resp(line);
			valid = true;
		} else if(nt == 2 || nt == 3){
			if(nt == 3) audio_set_card_device(token[2]);
			else        audio_set_card_device("");
			audio_set_backend(audio_parse_backend(token[1]));
			valid = true;
		} else if(nt == 6 && strcmp(token[1], "apply") == 0 &&
		          strcmp(token[2], "spk") == 0 && strcmp(token[4], "mic") == 0){
			// The requested configuration, applied: detach what is open for a
			// direction that changed, attach what was asked for.  A Bluetooth
			// direction needs a device chosen and connected first, so that is
			// done here rather than leaving the audio pointing at a PCM that
			// does not exist yet.
			AudioRoute want_out = audio_parse_route(token[3]);
			AudioRoute want_in  = audio_parse_route(token[5]);
			char line[200];
			bool bad = false;

			for(int d = 0; d < 2; d++){
				AudioRoute r = d ? want_in : want_out;
				if(r != AROUTE_BT) continue;
				const char *mac = d ? bt_in_mac() : bt_out_mac();
				const char *nm  = d ? bt_in_name() : bt_out_name();
				if(mac[0] == 0){
					snprintf(line, sizeof(line),
					         "%s: no Bluetooth device chosen - use Configure Bluetooth first",
					         d ? "microphone" : "speaker");
					gui_cmd_resp(line);
					bad = true;
					continue;
				}
				if(!bt_available()){
					gui_cmd_resp(bt_available_why());
					bad = true;
					continue;
				}
				if(!bt_connect_sync(mac, 12)){
					snprintf(line, sizeof(line),
					         "%s: %s did not answer - it will be attached when it does",
					         d ? "microphone" : "speaker", nm[0] ? nm : mac);
					gui_cmd_resp(line);
				}
			}
			if(bad){
				// Leave the direction that could not be configured where it is,
				// rather than silently moving the audio somewhere it was not
				// asked for.
				if(want_out == AROUTE_BT && bt_out_mac()[0] == 0) want_out = audio_get_route(true);
				if(want_in  == AROUTE_BT && bt_in_mac()[0]  == 0) want_in  = audio_get_route(false);
			}

			audio_apply_routes(want_out, want_in);

			{
				char l2[48];
				snprintf(l2, sizeof(l2), "spk %s", audio_route_name(want_out));
				save_line_to_config("spk", l2);
				snprintf(l2, sizeof(l2), "mic %s", audio_route_name(want_in));
				save_line_to_config("mic", l2);
			}

			{
				char so[64], si[64];
				if(want_out == AROUTE_BT && bt_out_name()[0])
					snprintf(so, sizeof(so), "bluetooth (%s)", bt_out_name());
				else
					snprintf(so, sizeof(so), "%s", audio_route_name(want_out));
				if(want_in == AROUTE_BT && bt_in_name()[0])
					snprintf(si, sizeof(si), "bluetooth (%s)", bt_in_name());
				else
					snprintf(si, sizeof(si), "%s", audio_route_name(want_in));
				snprintf(line, sizeof(line), "speaker: %s, microphone: %s", so, si);
				gui_cmd_resp(line);
			}
			valid = true;
		}
	}

	// The two routes, as config.txt records them ("spk bluetooth", "mic
	// soundcard").  Written by the interface's Configure Audio and by the
	// Bluetooth commands, so a restart comes back with the same arrangement.
	// Exactly these two-word forms: nothing else begins "spk" or "mic".
	if((strcmp(token[0], "spk") == 0 || strcmp(token[0], "mic") == 0) && nt == 2){
		const char *n = token[1];
		// "phone"/"android" belong on this list as much as the rest: without them
		// a "mic phone" line in config.txt was dropped without a word, so a
		// station whose microphone had been set to the phone came back from every
		// restart on the sound card - and transmitted a keyed carrier with
		// nothing on it while the phone's audio was thrown away.
		bool known = strcasecmp(n, "mute") == 0 || strcasecmp(n, "none") == 0 ||
		             strcasecmp(n, "browser") == 0 || strcasecmp(n, "web") == 0 ||
		             strcasecmp(n, "phone") == 0 || strcasecmp(n, "android") == 0 ||
		             strcasecmp(n, "soundcard") == 0 || strcasecmp(n, "card") == 0 ||
		             strcasecmp(n, "alsa") == 0 || strcasecmp(n, "bluetooth") == 0 ||
		             strcasecmp(n, "bt") == 0 || strcasecmp(n, "pulse") == 0;
		if(known){
			bool out = (token[0][0] == 's');
			audio_set_route(out, audio_parse_route(n));
			valid = true;
		}
	}

	// Microphone level and speaker volume: the two controls beside the meters.
	// Percentages, and applied here on the Pi, between the microphone and the
	// modulator / between the decoder and the speaker.
	//
	//   micgain PCT   0-200 (percent of unity: 100 = 0 dB, 200 = +6 dB)
	//   spkvol PCT    0-100 (0 = silent, 100 = full scale)
	if(strncmp(token[0],"micgain",7)==0){
		if(nt == 1){
			char line[120];
			double g = audio_get_mic_gain();
			snprintf(line, sizeof(line), "microphone gain %d%% (%+.1f dB)",
			         (int)(g * 100.0 + 0.5), 20.0 * log10(g > 0.001 ? g : 0.001));
			gui_cmd_resp(line);
			valid = true;
		} else if(nt == 2){
			int pct = atoi(token[1]);
			if(pct < 0) pct = 0;
			if(pct > 200) pct = 200;
			audio_set_mic_gain(pct / 100.0);
			char line[64];
			snprintf(line, sizeof(line), "micgain %d", pct);
			save_line_to_config("micgain", line);
			snprintf(line, sizeof(line), "microphone gain %d%% (%+.1f dB)", pct,
			         20.0 * log10(pct / 100.0 > 0.001 ? pct / 100.0 : 0.001));
			gui_cmd_resp(line);
			valid = true;
		}
	}
	if(strncmp(token[0],"spkvol",6)==0){
		if(nt == 1){
			char line[120];
			double v = audio_get_spk_vol();
			snprintf(line, sizeof(line), "speaker volume %d%% (%+.1f dB)",
			         (int)(v * 100.0 + 0.5), 20.0 * log10(v > 0.001 ? v : 0.001));
			gui_cmd_resp(line);
			valid = true;
		} else if(nt == 2){
			int pct = atoi(token[1]);
			if(pct < 0) pct = 0;
			if(pct > 100) pct = 100;
			audio_set_spk_vol(pct / 100.0);
			char line[64];
			snprintf(line, sizeof(line), "spkvol %d", pct);
			save_line_to_config("spkvol", line);
			snprintf(line, sizeof(line), "speaker volume %d%% (%+.1f dB)", pct,
			         20.0 * log10(pct / 100.0 > 0.001 ? pct / 100.0 : 0.001));
			gui_cmd_resp(line);
			valid = true;
		}
	}

	// The sound card's own hardware controls: "mixer" lists them, and
	// "mixer PCM 100" sets one.  The value is a percentage of whatever that
	// control allows, so one command works for any card.  It is written to
	// config.txt like the other settings, which also means it is applied again
	// at the next start - and the app re-applies it whenever it opens the
	// audio, so a card that drops off USB and returns gets it back.
	if(strncmp(token[0],"mixer",5)==0){
		int n = audio_alsa_mixer_count();
		if(nt == 1){
			char list[256];
			int used = 0;
			if(n == 0){
				snprintf(list, sizeof(list), "no sound card");
			}
			for(int i = 0; i < n && used < (int)sizeof(list) - 24; i++){
				used += snprintf(list + used, sizeof(list) - (size_t)used, "%s%s=%d%s",
					i ? " " : "", audio_alsa_mixer_name(i), audio_alsa_mixer_pct(i),
					audio_alsa_mixer_is_capture(i) ? "(mic)" : "");
			}
			gui_cmd_resp(list);
			valid = true;
		}
		if(nt == 3){
			int pct = atoi(token[2]);
			if(strncmp(token[2],"on",2) == 0)  pct = 100;
			if(strncmp(token[2],"off",3) == 0) pct = 0;
			// The number from "mixer" (names may contain spaces, which the
			// command line cannot carry).  A spaceless name is accepted too.
			int idx = -1;
			bool numeric = (token[1][0] != 0);
			for(const char *p = token[1]; *p; p++)
				if(*p < '0' || *p > '9') numeric = false;
			if(numeric){
				idx = atoi(token[1]);
			} else {
				for(int i = 0; i < n; i++)
					if(strcmp(audio_alsa_mixer_name(i), token[1]) == 0) idx = i;
			}
			if(idx < 0 || idx >= n){
				gui_cmd_resp("no such control");
			} else {
				if(audio_alsa_mixer_set(audio_alsa_mixer_name(idx), pct)){
					char line[64];
					snprintf(line, sizeof(line), "mixer %d %d", idx, pct);
					char key[48];
					snprintf(key, sizeof(key), "mixer %d", idx);
					save_line_to_config(key, line);
					valid = true;
				} else {
					gui_cmd_resp("the card refused that setting");
				}
			}
		}
	}

	// Listen to all addresses (not just ours and the broadcast address)
	if(strncmp(token[0],"la",2)==0){
		if(nt == 2){
			bool on = (strncmp(token[1],"on",2)==0)||(strncmp(token[1],"all",3)==0);
			m17_txrx_listen_all(on);
			valid = true;
		}
	}

	// Recording: writes what the receiver decodes - the same audio that goes to
	// the speaker, whichever decoder produced it - as a mono 8 kHz WAV, so a
	// classic example and a neural one can be compared directly.  Deliberately
	// not kept in config.txt: a station that started recording itself after a
	// reboot would fill its own disk.
	//
	//   record            is it recording, and where
	//   record on [file]  start (default rx-<date>-<time>.wav in the working directory)
	//   record off        close it
	if(strncmp(token[0],"record",6)==0){
		if(nt == 1){
			if(rec_active()){
				unsigned long n; unsigned int sec;
				char line[200];
				rec_stats(&n, &sec);
				snprintf(line, sizeof(line), "recording to %s at %d Hz, %u s",
				         rec_file(), rec_rate(), sec);
				gui_cmd_resp(line);
			} else if(rec_file()[0]){
				char line[200];
				snprintf(line, sizeof(line), "not recording; last file %s", rec_file());
				gui_cmd_resp(line);
			} else {
				gui_cmd_resp("not recording: record on starts, record off stops");
			}
			valid = true;
		} else if(strncmp(token[1],"on",2)==0){
			char line[200];
			// The rate follows the decoder in use: FARGAN synthesises at 16 kHz and
			// that is what is worth keeping, the classic decoder gives 8 kHz.
			int rate = fargan_rx_enabled() ? 16000 : 8000;
			if(rec_start(nt >= 3 ? token[2] : NULL, rate))
				snprintf(line, sizeof(line), "recording to %s at %d Hz", rec_file(), rec_rate());
			else
				snprintf(line, sizeof(line), "cannot record: %s", rec_error());
			gui_cmd_resp(line);
			valid = true;
		} else if(strncmp(token[1],"off",3)==0){
			char line[200];
			rec_stop();
			snprintf(line, sizeof(line), "recording stopped: %s", rec_file());
			gui_cmd_resp(line);
			valid = true;
		}
	}

	// The neural receiver: M17-Project/Codec2-mod-neural replaces only the
	// receiver's synthesis step, so the same 8-byte Codec2 3200 frames come back
	// at 16 kHz instead of 8 kHz.  The bitstream, the transmitter and every other
	// station are untouched, and it costs about half a core of the Pi while
	// receiving - hence a setting rather than the default.
	//
	//   fargan          is it built in, and is it on
	//   fargan on       receive through FARGAN (16 kHz, decimated to the audio path)
	//   fargan off      back to the classic decoder
	if(strncmp(token[0],"fargan",6)==0){
		if(nt == 1){
			if(!fargan_rx_compiled())
				gui_cmd_resp("neural receiver not built in (make NEURAL=1 CODEC2_BACKEND=mod)");
			else
				gui_cmd_resp(fargan_rx_enabled() ? "neural receiver on: FARGAN synthesises received frames"
				                                 : "neural receiver off: the classic decoder");
			valid = true;
		} else if(nt == 2 && (strncmp(token[1],"on",2)==0 || strncmp(token[1],"off",3)==0)){
			bool on = (strncmp(token[1],"on",2)==0);
			if(on && !fargan_rx_compiled()){
				gui_cmd_resp("neural receiver not built in (make NEURAL=1 CODEC2_BACKEND=mod)");
			} else {
				fargan_rx_set(on);
				save_line_to_config("fargan", on ? "fargan on" : "fargan off");
				gui_cmd_resp(on ? "neural receiver on: FARGAN at 16 kHz, decimated to the audio path"
				                : "neural receiver off: the classic decoder");
			}
			valid = true;
		}
	}

	// Bluetooth audio.  Output and microphone are chosen *separately*: an A2DP
	// speaker for the sound while the microphone stays on the sound card, or a
	// headset for one direction and another device for the other.
	//
	//   bt                    what is known, and what is in use
	//   bt spk MAC            output through that device (A2DP)
	//   bt spk MAC sco        output through it as HFP instead
	//   bt spk off            output back to the sound card
	//   bt mic MAC            microphone through that device (HFP)
	//   bt mic off            microphone back to the sound card
	//   bt use MAC speaker    the old form: output only
	//   bt use MAC headset    the old form: both directions on that device
	//   bt pair MAC           pair and trust a device the scan found
	//   bt scan               look for devices for 20 s
	//   bt off                both directions back to the sound card
	//   btspk MAC [a2dp|sco]  the config.txt form: remembers, does not connect
	//   btmic MAC             the config.txt form for the microphone
	//   btdev MAC ROLE        the older config.txt form, still read
	if(strncmp(token[0],"bt",2)==0){
		char line[160];

		if(strncmp(token[0],"btspk",5)==0 || strncmp(token[0],"btmic",5)==0){
			// Startup forms.  No connection attempt: bluealsa reconnects to a
			// trusted device itself, and a device that is not there leaves the
			// audio on the card rather than going silent.
			bool spk = (strncmp(token[0],"btspk",5) == 0);
			if(nt == 2 && strncmp(token[1],"off",3) == 0){
				if(spk) bt_set_output(NULL, NULL);
				else    bt_set_input(NULL);
			} else if(spk && (nt == 2 || nt == 3)){
				bt_set_output(token[1], nt == 3 ? token[2] : "a2dp");
			} else if(!spk && nt == 2){
				bt_set_input(token[1]);
			}
			bt_sync_routes();
			valid = true;
		} else if(strncmp(token[0],"btdev",5)==0){
			if(nt == 2 && strncmp(token[1],"off",3)==0){
				bt_clear();
				bt_sync_routes();
				valid = true;
			} else if(nt == 3 && bt_set_active(token[1], token[2])){
				bt_sync_routes();
				valid = true;
			}
		} else if(nt == 1){
			if(!bt_available()){
				gui_cmd_resp(bt_available_why());
			} else {
				bt_refresh();
				snprintf(line, sizeof(line), "adapter %s%s", bt_adapter(),
				         bt_powered() ? "" : " (powered off)");
				gui_cmd_resp(line);
				snprintf(line, sizeof(line), "speaker: %s%s",
				         bt_out_mac()[0] ? (bt_out_name()[0] ? bt_out_name() : bt_out_mac())
				                         : "the sound card",
				         bt_out_mac()[0] ? (bt_out_connected() ? "  (connected)" : "  (not connected)")
				                         : "");
				gui_cmd_resp(line);
				snprintf(line, sizeof(line), "microphone: %s%s",
				         bt_in_mac()[0] ? (bt_in_name()[0] ? bt_in_name() : bt_in_mac())
				                        : "the sound card",
				         bt_in_mac()[0] ? (bt_in_connected() ? "  (connected)" : "  (not connected)")
				                        : "");
				gui_cmd_resp(line);
				for(int i = 0; i < bt_count(); i++){
					snprintf(line, sizeof(line), "%s  %s%s%s%s%s", bt_mac(i), bt_name(i),
					         bt_connected(i) ? "  (connected)" : "",
					         bt_paired(i)    ? "" : "  (not paired)",
					         strcmp(bt_out_mac(), bt_mac(i)) == 0 ? "  [speaker]" : "",
					         strcmp(bt_in_mac(),  bt_mac(i)) == 0 ? "  [mic]" : "");
					gui_cmd_resp(line);
				}
				if(bt_count() == 0) gui_cmd_resp("nothing known yet: bt scan looks for devices");
			}
			valid = true;
		} else if(nt == 2 && strncmp(token[1],"scan",4)==0){
			if(!bt_available()){
				gui_cmd_resp(bt_available_why());
			} else {
				bt_scan_async();
				gui_cmd_resp("looking for devices for 20s, then run bt again");
			}
			valid = true;
		} else if(nt == 2 && strncmp(token[1],"off",3)==0){
			bt_clear();
			bt_sync_routes();
			save_line_to_config("btspk", "btspk off");
			save_line_to_config("btmic", "btmic off");
			gui_cmd_resp("speaker and microphone back on the sound card");
			valid = true;
		} else if(nt >= 4 && strncmp(token[1],"sel",3)==0 &&
		          (strncmp(token[2],"spk",3)==0 || strncmp(token[2],"mic",3)==0)){
			// "bt sel spk MAC" / "bt sel mic MAC": choose and connect a device
			// WITHOUT moving the audio there.  This is what the interface's
			// Configure Bluetooth does - picking a device and connecting it -
			// leaving Configure Audio to attach it, so nothing changes under a
			// half-filled request.
			bool spk = (strncmp(token[2],"spk",3) == 0);
			const char *mac = token[3];
			if(strncmp(mac,"off",3) == 0){
				if(spk){ bt_set_output(NULL, NULL); save_line_to_config("btspk", "btspk off"); }
				else   { bt_set_input(NULL);        save_line_to_config("btmic", "btmic off"); }
				gui_cmd_resp(spk ? "speaker device cleared (the routes are unchanged)"
				                 : "microphone device cleared (the routes are unchanged)");
				valid = true;
			} else if(!bt_valid_mac(mac)){
				gui_cmd_resp("not a Bluetooth address (xx:xx:xx:xx:xx:xx)");
			} else if(!bt_available()){
				gui_cmd_resp(bt_available_why());
			} else {
				bool wanted_a2dp = (nt < 5 || strcmp(token[4], "sco") != 0);
				if(spk) bt_set_output(mac, wanted_a2dp ? "a2dp" : "sco");
				else    bt_set_input(mac);
				bool up = bt_connect_sync(mac, 12);
				if(spk){
					// bt_set_output() may have chosen HFP for a device with no
					// A2DP, so save what is really in use, not what was asked.
					snprintf(line, sizeof(line), "btspk %s %s", mac, bt_out_profile());
					save_line_to_config("btspk", line);
				} else {
					snprintf(line, sizeof(line), "btmic %s", mac);
					save_line_to_config("btmic", line);
				}
				if(spk && wanted_a2dp && strcmp(bt_out_profile(), "sco") == 0)
					snprintf(line, sizeof(line),
					         "speaker device chosen (HFP: this device has no A2DP)%s - Configure Audio attaches it",
					         up ? "" : ", not answering yet");
				else
					snprintf(line, sizeof(line), "%s device chosen%s - Configure Audio attaches it",
					         spk ? "speaker" : "microphone",
					         up ? "" : ", not answering yet");
				gui_cmd_resp(line);
				valid = true;
			}
		} else if(nt == 3 && strncmp(token[1],"pair",4)==0){
			// A device found by a scan is only discovered: nothing will connect
			// it until it is paired, and a one-shot bluetoothctl registers no
			// pairing agent unless it is asked for one.
			const char *mac = token[2];
			if(!bt_valid_mac(mac)){
				gui_cmd_resp("not a Bluetooth address (xx:xx:xx:xx:xx:xx)");
			} else if(!bt_available()){
				gui_cmd_resp(bt_available_why());
			} else if(bt_pair_sync(mac, 20)){
				bt_refresh();
				gui_cmd_resp("paired and trusted - now choose speaker or microphone");
			} else {
				gui_cmd_resp("pairing failed: the device may want a PIN, or be out of range");
			}
			valid = true;
		} else if(nt >= 3 && (strncmp(token[1],"spk",3)==0 || strncmp(token[1],"mic",3)==0)){
			// One direction at a time: "bt spk MAC" moves the speaker only, so
			// the microphone can stay where it is (on the card, or on another
			// device).  "bt spk MAC sco" asks for HFP instead of A2DP.
			bool spk = (strncmp(token[1],"spk",3) == 0);
			const char *mac = token[2];
			if(nt == 2 || (nt == 3 && strncmp(mac,"off",3) == 0)){
				if(spk){ bt_set_output(NULL, NULL); save_line_to_config("btspk", "btspk off"); }
				else    { bt_set_input(NULL);       save_line_to_config("btmic", "btmic off"); }
				bt_sync_routes();
				gui_cmd_resp(spk ? "speaker back on the sound card" : "microphone back on the sound card");
				valid = true;
			} else if(!bt_valid_mac(mac)){
				gui_cmd_resp("not a Bluetooth address (xx:xx:xx:xx:xx:xx)");
			} else if(!bt_available()){
				gui_cmd_resp(bt_available_why());
			} else {
				const char *prof = (nt == 4 && bt_valid_profile(token[3])) ? token[3] : "a2dp";
				if(spk){ bt_set_output(mac, prof); prof = bt_out_profile(); }
				else   { bt_set_input(mac); }
				bool up = bt_connect_sync(mac, 12);
				bt_sync_routes();
				bt_connect_async(mac);
				if(spk){
					snprintf(line, sizeof(line), "btspk %s %s", mac, prof);
					save_line_to_config("btspk", line);
					if(!up) gui_cmd_resp("the device did not answer yet; the audio will move when it does");
					else    gui_cmd_resp(prof[0] == 's' ? "speaker: HFP output (8 kHz)"
					                                   : "speaker: A2DP output (the microphone stays where it is)");
				} else {
					snprintf(line, sizeof(line), "btmic %s", mac);
					save_line_to_config("btmic", line);
					if(!up) gui_cmd_resp("the device did not answer yet; the microphone will move when it does");
					else    gui_cmd_resp("microphone: through the headset (HFP, 8 kHz)");
				}
				valid = true;
			}
		} else if(nt == 3 || (nt == 4 && strncmp(token[1],"use",3) == 0)){
			// "bt <mac> speaker" and "bt use <mac> speaker" are both accepted,
			// and are the shorthand for "put this direction on this device".
			const char *mac  = token[nt - 2];
			const char *role = token[nt - 1];
			if(!bt_valid_mac(mac)){
				gui_cmd_resp("not a Bluetooth address (xx:xx:xx:xx:xx:xx)");
			} else if(!bt_available()){
				gui_cmd_resp(bt_available_why());
			} else if(!bt_set_active(mac, role)){
				gui_cmd_resp("the role is speaker or headset");
			} else {
				bool up = bt_connect_sync(mac, 12);
				bt_sync_routes();
				bt_connect_async(mac);
				if(!up) gui_cmd_resp("the device did not answer; the audio stays where it was");
				if(strcmp(role,"speaker") == 0){
					snprintf(line, sizeof(line), "btspk %s %s", mac, bt_out_profile());
					save_line_to_config("btspk", line);
				} else {
					snprintf(line, sizeof(line), "btspk %s sco", mac);
					save_line_to_config("btspk", line);
					snprintf(line, sizeof(line), "btmic %s", mac);
					save_line_to_config("btmic", line);
				}
				gui_cmd_resp(strcmp(role,"headset") == 0
				             ? "headset: talk and listen through it (HFP, 8 kHz)"
				             : "speaker: the microphone stays where it is");
				valid = true;
			}
		}
	}

	// Stop transmitting.  Exactly "b": any token starting with b would also
	// stop the transmitter, so "ba" and "bt" used to do it as well.
	if(strcmp(token[0],"b")==0){
		m17_txrx_ptt_off();
		valid = true;
	}

#ifdef HAVE_SIM
	// Simulated radio controls: sim loop on|off, sim realtime on|off, sim noise 0.01
	if(strncmp(token[0],"sim",3)==0){
		if(nt == 3){
			if(strncmp(token[1],"loop",4)==0)
				sim_set_loopback(strncmp(token[2],"on",2)==0);
			if(strncmp(token[1],"realtime",8)==0)
				sim_set_realtime(strncmp(token[2],"on",2)==0);
			if(strncmp(token[1],"noise",5)==0)
				sim_set_noise(atof(token[2]));
			valid = true;
		}
	}
#endif

	// Help
	if(strncmp(token[0],"h",1)==0){
		help();
		valid = true;
	}

	if(valid == false){
		gui_cmd_resp("Invalid command");
	}
	else{
		gui_cmd_resp("OK");
	}
	gui_update();
}
void mmi_load_file(const char *name){
	FILE *fp;
	char text[256];
	if((fp=fopen(name,"r"))!=NULL){
		while(fgets(text,255,fp)!= NULL){
			//printf("%s",text);
			mmi_parse(text);
		}
	}else{
		char text[1024];
		sprintf(text,"Unable to open %s\n",name);
		gui_cmd_resp(text);
	}
}
