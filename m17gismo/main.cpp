//
// main.cpp - m17web
//
// Startup, command line handling and thread management.  All user interface
// work happens in the presentation layer (webui.cpp or ui_console.cpp).
//
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "m17defines.h"
#include "m17_ax25.h"
#include "bluetooth.h"

static uint8_t m_meta[16];
static pthread_t m_m17_threads[3];   // radio, command loop, hardware PTT
static RadioType m_radio_type;

#define VERSION "1.0-web"

//
// Command line parameters
//
void command_line(int c, char **argv){
	// Default radio type: prefer real hardware when it is compiled in
#if defined(HAVE_PLUTO)
	m_radio_type = RADIO_TYPE_PLUTO;
#elif defined(HAVE_SIM)
	m_radio_type = RADIO_TYPE_SIM;
#else
	m_radio_type = RADIO_TYPE_LIME;
#endif

	for( int i = 1; i < c; i++){
		if(strcmp(argv[i],"-p")==0 && i + 1 < c){
#ifdef HAVE_PLUTO
			pluto_set_ip_address(argv[i+1]);
#endif
			i++;
			continue;
		}
		if(strcmp(argv[i],"-r")==0 && i + 1 < c){
			if(strcmp(argv[i+1],"pluto")==0){
#ifdef HAVE_PLUTO
				m_radio_type = RADIO_TYPE_PLUTO;
#else
				fprintf(stderr,"m17web: Pluto support not compiled in\n");
#endif
			}
			if(strcmp(argv[i+1],"lime")==0){
#ifdef HAVE_LIME
				m_radio_type = RADIO_TYPE_LIME;
#else
				fprintf(stderr,"m17web: LimeSDR support not compiled in\n");
#endif
			}
			if(strcmp(argv[i+1],"sim")==0){
#ifdef HAVE_SIM
				m_radio_type = RADIO_TYPE_SIM;
#else
				fprintf(stderr,"m17web: simulator not compiled in\n");
#endif
			}
			i++;
			continue;
		}
		if(strcmp(argv[i],"-w")==0 && i + 1 < c){
			ui_set_web_port(atoi(argv[i+1]));
			i++;
			continue;
		}
		if(strcmp(argv[i],"-www")==0 && i + 1 < c){
			ui_set_www_dir(argv[i+1]);
			i++;
			continue;
		}
		if(strcmp(argv[i],"-a")==0 && i + 1 < c){
			// browser | pulse | both | none | file:in.raw:out.raw
			char spec[512];
			snprintf(spec, sizeof(spec), "%s", argv[i+1]);
			if(strncmp(spec, "file", 4) == 0){
				char *in = strchr(spec, ':');
				char *out = in ? strchr(in + 1, ':') : NULL;
				if (in)  { *in++ = 0; }
				if (out) { *out++ = 0; }
				audio_file_set(in, out);
				audio_set_backend(AUDIO_FILE);
			} else {
				audio_set_backend(audio_parse_backend(spec));
			}
			i++;
			continue;
		}
		if(strcmp(argv[i],"-h")==0 || strcmp(argv[i],"--help")==0){
			printf("m17web %s - M17 SDR with a web interface\n\n", VERSION);
			printf("usage: m17web [options]\n\n");
			printf("  -r pluto|lime|sim   radio back end (sim = no hardware, TX looped to RX)\n");
			printf("  -p xxx.xxx.xxx.xxx  IPv4 address of the Pluto (default: pluto.local)\n");
			printf("  -w port             web UI port (default %d)\n", ui_get_web_port());
			printf("  -www dir            directory holding the web assets (default www)\n");
			printf("  -a backend          audio routing: browser (default), pulse, both, none,\n");
			printf("                      or file:in.raw:out.raw for headless testing\n");
			printf("  -h                  this help\n\n");
			printf("Commands are typed in the browser (see the help pane) or in config.txt\n");
			exit(0);
		}
	}
}

int main( int c, char **argv ){
	M17Type type;
	// Line buffered so the startup banner reaches a log file promptly
	setvbuf(stdout, NULL, _IOLBF, 0);
	type.p_s      = CCT_STREAM; // A stream
	type.dt       = DATA_VOICE; // of voice
	type.et       = ENC_NONE ;  // With no encryption
	type.est      = ENC_SUB ;   // empty
	type.can      = CAN_NUM;    // empty
	type.reserved = RES_RES;    // not used

    // Load any command line parameters
	command_line( c, argv );

	radio_open(m_radio_type);

	m17_prbs9_init();
	m17_crc_init();
	m17_init_conv();
	m17_init_de_correlate();
	m17_dsp_init();
	m17_fmt_init();
	m17_golay_init();
	m17_txrx_init();
	m17_rx_sync_init();
	buff_open();
	gps_open();

#ifdef __TEST__
	printf("TEST MODE TEST MODE TEST MODE TEST MODE\n");
	m17_test_init();
#endif
	m17_mod_init();

	// Default configuration, basic radio
	m17_db_set_chan_type(DRTOAS);
	m17_txrx_set_src_add( "YOURCALL ");
	m17_txrx_set_dest_add("HISCALL  ");
	m17_txrx_set_brd_add();
	m17_txrx_set_type(type);
	m17_txrx_set_meta(m_meta);

	radio_set_rx_frequency(434000000);
	radio_set_tx_frequency(434000000);

	// Start up in receive mode
	m17_txrx_ptt_off();

	// Initialise the user interface (this also starts the web server)
	gui_open();
    gui_update();

	// AX.25 over M17 starts closed; config.txt decides, if it says anything
	m17_ax25_init();

	// Load initialisation
	mmi_load_file("config.txt");

	// Keep the Bluetooth view honest: a speaker that is switched off must not
	// keep showing "connected", and the device list has to pick up what a scan
	// found.  A no-op where Bluetooth is not available.
	bt_start_monitor();

	if(pthread_create( &m_m17_threads[0], NULL, m17_txrx_threads, NULL ) != 0 )
    {
        printf("Unable to start m17 net threads\n");
        return(0);
    }

	// Poll the hardware PTT switch: a no-op on a station without one
	if(pthread_create( &m_m17_threads[2], NULL, m17_txrx_ptt_thread, NULL ) != 0 )
    {
        printf("Unable to start the hardware PTT thread\n");
    }

	// For the web UI this waits for a browser 'q' or a signal; for the console
	// build it reads commands from the keyboard.
	if(pthread_create( &m_m17_threads[1], NULL, ui_command_loop, NULL ) != 0 )
    {
        printf("Unable to start m17 command thread\n");
        return(0);
    }

	// Make sure we don't terminate until all threads have completed
	printf("m17web: waiting for the radio thread to stop\n");
    pthread_join(m_m17_threads[0], NULL);
	printf("m17web: waiting for the command thread to stop\n");
    pthread_join(m_m17_threads[1], NULL);

	printf("m17web: disconnecting reflector\n");
    m17_net_disconnect_from_reflector();
    sleep(1);
	printf("m17web: closing network\n");
	m17_net_close();
	printf("m17web: closing radio\n");
	radio_close();
	printf("m17web: closing UI\n");
	gui_close();
	printf("m17web: closing gps/buffers\n");
	gps_close();
	buff_close();
	printf("m17web: bye\n");
	return(0);
}
