#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
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
	if(strncmp(token[0],"audio",5)==0){
		if(nt == 2 || nt == 3){
			if(nt == 3) audio_alsa_set_device(token[2]);
			audio_set_backend(audio_parse_backend(token[1]));
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

	// Bluetooth audio: a speaker (A2DP, playback only - the microphone stays on
	// the sound card) or a headset (HFP, both directions at M17's own 8 kHz,
	// so nothing is lost against Codec2).  Both need bluealsa on the host; the
	// station is unchanged without it and this says why.
	//
	//   bt                    what is known, and what is in use
	//   bt use MAC speaker    trust it, connect, send the audio to it
	//   bt use MAC headset    ...as a headset, microphone included
	//   bt scan               look for devices for 20 s
	//   bt pair MAC           pair and trust a device the scan found
	//   bt off                back to the sound card
	//   btdev MAC ROLE        the config.txt form: remembers, does not connect
	if(strncmp(token[0],"bt",2)==0){
		if(strncmp(token[0],"btdev",5)==0){
			char play[160], cap[160];
			if(nt == 2 && strncmp(token[1],"off",3)==0){
				bt_clear();
				audio_alsa_set_device("");
				audio_alsa_set_capture_device("");
				valid = true;
			} else if(nt == 3 && bt_set_active(token[1], token[2])){
				// Startup: no connection attempt here.  bluealsa reconnects to a
				// trusted device itself, and if it is not there the audio falls
				// back to the card instead of going silent.
				bt_alsa_devices(play, sizeof(play), cap, sizeof(cap));
				audio_alsa_set_device(play);
				if(cap[0]) audio_alsa_set_capture_device(cap);
				else       audio_alsa_capture_follow_card();
				valid = true;
			}
		} else if(nt == 1){
			char line[160];
			if(!bt_available()){
				gui_cmd_resp(bt_available_why());
			} else {
				bt_refresh();
				snprintf(line, sizeof(line), "adapter %s%s", bt_adapter(),
				         bt_powered() ? "" : " (powered off)");
				gui_cmd_resp(line);
				if(bt_active_mac()[0])
					snprintf(line, sizeof(line), "using %s %s%s", bt_active_role(),
					         bt_active_name()[0] ? bt_active_name() : bt_active_mac(),
					         bt_active_connected() ? "  (connected)" : "  (not connected)");
				else
					snprintf(line, sizeof(line), "using the sound card");
				gui_cmd_resp(line);
				for(int i = 0; i < bt_count(); i++){
					snprintf(line, sizeof(line), "%s  %s%s%s", bt_mac(i), bt_name(i),
					         bt_connected(i) ? "  (connected)" : "",
					         bt_paired(i)    ? "" : "  (not paired)");
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
			audio_alsa_set_device("");
			audio_alsa_set_capture_device("");
			audio_set_backend(audio_parse_backend("alsa"));
			save_line_to_config("btdev", "btdev off");
			gui_cmd_resp("audio back on the sound card");
			valid = true;
		} else if(nt == 3 && strncmp(token[1],"pair",4)==0){
			// A device found by a scan is only discovered: nothing will connect
			// it until it is paired, and a one-shot bluetoothctl registers no
			// pairing agent unless it is asked for one.  Pair and trust here;
			// choosing speaker or headset afterwards does the connect.
			const char *mac = token[2];
			if(!bt_valid_mac(mac)){
				gui_cmd_resp("not a Bluetooth address (xx:xx:xx:xx:xx:xx)");
			} else if(!bt_available()){
				gui_cmd_resp(bt_available_why());
			} else if(bt_pair_sync(mac, 20)){
				bt_refresh();
				gui_cmd_resp("paired and trusted - now choose speaker or headset");
			} else {
				gui_cmd_resp("pairing failed: the device may want a PIN, or be out of range");
			}
			valid = true;
		} else if(nt == 3 || (nt == 4 && strncmp(token[1],"use",3) == 0)){
			// "bt <mac> speaker" and "bt use <mac> speaker" are both accepted.
			char play[160], cap[160], line[96];
			const char *mac  = token[nt - 2];
			const char *role = token[nt - 1];
			if(!bt_valid_mac(mac)){
				gui_cmd_resp("not a Bluetooth address (xx:xx:xx:xx:xx:xx)");
			} else if(!bt_available()){
				gui_cmd_resp(bt_available_why());
			} else if(!bt_set_active(mac, role)){
				gui_cmd_resp("the role is speaker or headset");
			} else {
				// Connect first: bluealsa only creates the PCM once the
				// profile is up, and pointing the audio at a PCM that is not
				// there yet would just fall back to the sound card.
				bool up = bt_connect_sync(mac, 12);
				bt_alsa_devices(play, sizeof(play), cap, sizeof(cap));
				audio_alsa_set_device(play);
				if(cap[0]) audio_alsa_set_capture_device(cap);
				else       audio_alsa_capture_follow_card();
				audio_set_backend(audio_parse_backend("alsa"));
				bt_connect_async(mac);   // and keep it connected from now on
				if(!up) gui_cmd_resp("the device did not answer; the audio stays on the sound card");
				snprintf(line, sizeof(line), "btdev %s %s", mac, role);
				save_line_to_config("btdev", line);
				gui_cmd_resp(cap[0] ? "headset: talk and listen through it (HFP, 8 kHz)"
				                    : "speaker: the microphone stays on the sound card");
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
