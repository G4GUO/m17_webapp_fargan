#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <memory.h>
#include <pthread.h>
#include <time.h>
#include "m17defines.h"
#include "fargan.h"   // receive-only neural synthesis; a stub unless FARGAN_RX
#include "recorder.h" // optional capture of the decoded audio as a WAV
// codec2 (or the modified codec through codec2mod/codec2_shim.h) is included by
// m17defines.h according to CODEC2_BACKEND.

typedef enum{PTT_RX,PTT_TX,PTT_CA,PTT_DP}PttType;
typedef enum{STATE_RX,STATE_TX,STATE_DP}NetState;

static volatile PttType m_ptt;        // set from the web UI and the hardware PTT thread
static struct CODEC2 *m_codec2;
static int m_nsam;
static int16_t m_amicsamples[160];
static int16_t m_aspksamples[160];
static scmplx  m_rsamples[1920];
static M17Type m_type;
static uint8_t m_meta[14];
// Set from other threads (mmi / web UI): volatile so loops actually re-read it
static volatile bool m_running;

// ---------------------------------------------------------------------------
// Changing the mode while the program is running
//
// The dispatcher below (m17_txrx_threads) calls the thread for the current mode
// synchronously, and those threads run until the program stops - which is why
// the mode could only ever be chosen at startup.  Each thread now remembers the
// mode generation it was started with and stops when it changes, so the
// dispatcher picks up the new mode without a restart.  A mode change also
// returns to receive: a transmission must not be carried across it.
// ---------------------------------------------------------------------------
static volatile int m_mode_gen;
#define M17_MODE_OK(gen) (m_running == true && (gen) == m_mode_gen)

int m17_txrx_mode_generation(void) {
    return m_mode_gen;
}

void m17_txrx_mode_changed(void) {
    m_ptt = PTT_RX;
    m_mode_gen++;
}
static NetState m_net_state = STATE_RX;
// Blank (silence) codec frames sent at the start of every transmission, see
// m17_tx_lead_in().  Even numbers only: two codec frames make one M17 sub frame.
static int m_tx_lead_frames = 4;              // 80 ms
static pthread_mutex_t m_lead_mtx = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// GNSS position in the LSF META field
//
// When the TYPE field's encryption subtype bits are 0b01 the META field carries
// the station's position (see META_* in m17defines.h). It is written just before
// the link setup frame goes out, and refreshed at superframe boundaries during
// the over - no more often than every 5 seconds, because the specification says
// a receiver may only be expected to notice a change that often.
// ---------------------------------------------------------------------------
static volatile bool m_gnss_tx;               // GNSS is in the META field
static volatile bool m_gnss_enable = true;    // the operator allows it on the air
static uint32_t      m_gnss_tx_ms;            // when it was last written

static uint32_t tx_now_ms(void){
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void m17_tx_meta_from_gps(bool force){
    if (!m_gnss_enable) {
        // Switched off from the console or the page: drop the position from the
        // META field so it stops being announced from the next frame onwards.
        if (m_gnss_tx) {
            memset(m_meta, 0, sizeof(m_meta));
            m_type.est = META_TEXT;
            m_gnss_tx = false;
        }
        return;
    }
    if (!gps_fix_fresh(30000)) {
        // No usable fix: announce nothing rather than a stale position.
        if (m_gnss_tx) {
            memset(m_meta, 0, sizeof(m_meta));
            m_type.est = META_TEXT;
            m_gnss_tx = false;
        }
        return;
    }
    uint32_t now = tx_now_ms();
    if (!force && (now - m_gnss_tx_ms) < 5000) return;
    GpsMsg g = m_gps;                       // copy: the reader thread updates it
    gps_encode(m_meta, &g);
    m_type.est = META_GNSS;
    m_gnss_tx  = true;
    m_gnss_tx_ms = now;
}

bool m17_tx_gnss(void){
    return m_gnss_tx;
}

bool m17_tx_gnss_enabled(void){
    return m_gnss_enable;
}

void m17_tx_set_gnss_enabled(bool on){
    m_gnss_enable = on;
    if (!on) m17_tx_meta_from_gps(true);   // clear it now, do not wait for a frame
}

int  m17_tx_lead_frames(void) { return m_tx_lead_frames; }
void m17_tx_set_lead_frames(int frames) {
    if (frames < 0) frames = 0;
    if (frames > 50) frames = 50;             // 1 s of silence is plenty
    pthread_mutex_lock(&m_lead_mtx);
    m_tx_lead_frames = frames;
    pthread_mutex_unlock(&m_lead_mtx);
}

//
// Leading silence.  A receiver has to acquire bit sync, the LICH and the link
// setup frame before a stream frame means anything, and at key-down the local
// microphone path is only just starting: a USB card takes tens of ms to hand
// over its first samples, and a browser adds a round trip.  Without this the
// first voice frames went out as gap, which sounds like the transmitter
// dropping samples.  Sending a couple of blank codec frames first moves that
// gap into the silence and gives the microphone buffers time to fill.
//
static void m17_tx_lead_in_ex(bool to_net){
    int frames = m17_tx_lead_frames();
    if (frames < 2) return;
    frames &= ~1;                             // whole sub frames only
    int16_t silence[160];
    uint8_t payload[16];
    memset(silence, 0, sizeof(int16_t) * (size_t)m_nsam);
    while (frames > 0) {
        memset(payload, 0, sizeof(payload));
        codec2_encode(m_codec2, &payload[0], silence);
        codec2_encode(m_codec2, &payload[8], silence);
        if (to_net) m17_send_stream_frame_to_net(payload);
        else        m17_send_stream_frame(payload);
        radio_power_update();
#ifdef M17WEB_DIAG
        fprintf(stdout, "diag-tx: lead-in blank frame payload=%02X%02X%02X%02X%02X%02X%02X%02X"
                        " (%d left) net=%d\n", payload[0], payload[1], payload[2], payload[3],
                payload[4], payload[5], payload[6], payload[7], frames - 2, to_net ? 1 : 0);
        fflush(stdout);
#endif
        // Real time, so the microphone has the same time to fill as the stream
        // takes to play: the point is to hide the start-up gap, not to send
        // silence as fast as the CPU can encode it.
        usleep(40000);
        frames -= 2;
    }
}

static void m17_tx_lead_in(void)         { m17_tx_lead_in_ex(false); }
static void m17_tx_lead_in_to_net(void)  { m17_tx_lead_in_ex(true); }

void m17_txrx_spkr_audio( uint8_t *data ){

   // Receive-only option: the same 8-byte Codec2 3200 frame, synthesised by
   // FARGAN instead of the sinusoidal model.  The frame is untouched, so the
   // station at the other end cannot tell which synthesiser is in use here.
   //
   // The synthesis costs about half a Pi 4 core and this runs on the radio
   // thread and on the reflector thread, so it is done elsewhere: the frame is
   // queued and whatever finished audio is ready is played.  Nothing is waiting
   // for the vocoder - a frame or two of latency is the price, which matters far
   // less than the demodulator falling behind.
   static int16_t last[320];          // zeros until the first block arrives
   if (fargan_rx_enabled()) {
      int16_t block8[160], block16[320];
      fargan_rx_push(data);
      if (fargan_rx_pull(block8, block16)) {
         memcpy(last, block8, sizeof(block8));
         // The audio path plays the 8 kHz version, but a recording keeps
         // FARGAN's own 16 kHz output: that is the resolution it synthesised at,
         // and decimating it only throws information away.
         rec_push(block16, 320, 16000);
         audio_output(block8, m_nsam);
         return;
      }
      // If nothing is ready yet - the first frames after switching on - keep the
      // audio stream fed with the previous block rather than dropping to silence.
      audio_output(last, m_nsam);
      return;
   }

   codec2_decode(m_codec2, m_aspksamples, data);
   memcpy(last, m_aspksamples, sizeof(int16_t) * (m_nsam < 320 ? m_nsam : 320));
   rec_push(m_aspksamples, m_nsam, 8000);      // the classic decoder is 8 kHz
   audio_output(m_aspksamples, m_nsam);
}

void *m17_txrx_net_thread(void *arg){

	m_net_state = STATE_RX;
    radio_receive();

    const int gen = m_mode_gen;                 // the mode this thread started for
    while(M17_MODE_OK(gen)){

    	if(m_net_state == STATE_RX){
		    int len = radio_receive_samples( m_rsamples, N_SAMPLES );
		    m17_dsp_rx( m_rsamples, len );
			radio_rssi_update();
			int n = buff_size();
			if( n > 15 ){
				uint8_t *b = buff_get();
				if(b != NULL){
					// We have something to transmit
			        radio_transmit();
			        m17_send_carrier();
			        m17_send_preamble();
			        m17_send_preamble();
			        m17_send_link_setup_frame_fm_net(b);
				    // Send Sub frame
				    m17_send_stream_frame_fm_net(b);
				    m_net_state = STATE_TX;
			    }
			}
        }

		if(m_net_state == STATE_TX){
			uint8_t *b = buff_get();
			if(b != NULL){
				// We have something to transmit
			    // Send Sub frame
			    m17_send_stream_frame_fm_net(b);
			    radio_power_update();
			}else{
				// Back to receive
			    // Send EOT
			    m17_send_eot();
			    usleep(40000);
			    // Go to receive
			    radio_receive();
			    m_net_state = STATE_RX;
			}
        }

        if(m_net_state == STATE_DP){

        }

    }
     // Close the system
    return arg;
}

void *m17_txrx_thread(void *arg){
    uint8_t payload[16];
    const int gen = m_mode_gen;                 // the mode this thread started for

    while(M17_MODE_OK(gen)){
        // Simplex transmit
		if( m_ptt == PTT_TX){
		    audio_mic_open();
		    usleep(120000);// Buffer 6 sound blocks
		    // Go to transmit
		    //radio_duplex();
		    radio_transmit();
	        // Send preamble
		    m17_send_carrier();
		    m17_send_preamble();
		    m17_send_preamble();
		    // Put the current position in the META field before announcing it
		    m17_tx_meta_from_gps(true);
		    // Send link setup
		    m17_send_link_setup_frame(m17_db_get_dst(), m17_db_get_src(), m_type, m_meta);
		    // A couple of blank codec frames before the voice (see m17_tx_lead_in)
		    m17_tx_lead_in();
		    // Everything the microphone captured while the radio was starting up
		    // is now stale: send live audio from here (see audio_input_flush).
		    audio_input_flush();
		    // Send sub frames of voice, 25 sub frames per sec
		    while((m_ptt == PTT_TX)&&M17_MODE_OK(gen)){
#ifdef M17WEB_DIAG
			    struct timespec d0, d1, d2, d3, d4;
			    static long t_audio, t_encode, t_send, t_pow, t_total, n_sub;
			    clock_gettime(CLOCK_MONOTONIC, &d0);
#endif
			    // Read Audio samples and encode using Codec 2
			    // Audio blocks are 20 ms but M17 chunks are 40 ms so 2 codec frames per m17 sub frame
			    audio_input(m_amicsamples, m_nsam);
#ifdef M17WEB_DIAG
			    clock_gettime(CLOCK_MONOTONIC, &d1);
#endif
			    codec2_encode(m_codec2, &payload[0], m_amicsamples);
			    // Read Audio samples and encode using Codec 2
			    audio_input(m_amicsamples, m_nsam);
			    codec2_encode(m_codec2, &payload[8], m_amicsamples);
#ifdef M17WEB_DIAG
			    clock_gettime(CLOCK_MONOTONIC, &d2);
#endif
			    // New superframe: the LICH is starting again, so this is the point
			    // at which the META field (and the position in it) may change.
			    if (m17_tx_lich_index() == 0) m17_tx_meta_from_gps(false);
			    // Send Sub frame
			    m17_send_stream_frame( payload );
#ifdef M17WEB_DIAG
			    clock_gettime(CLOCK_MONOTONIC, &d3);
#endif
			    radio_power_update();
#ifdef M17WEB_DIAG
			    clock_gettime(CLOCK_MONOTONIC, &d4);
			    t_audio  += (d1.tv_sec - d0.tv_sec) * 1000000 + (d1.tv_nsec - d0.tv_nsec) / 1000;
			    t_encode += (d2.tv_sec - d1.tv_sec) * 1000000 + (d2.tv_nsec - d1.tv_nsec) / 1000;
			    t_send   += (d3.tv_sec - d2.tv_sec) * 1000000 + (d3.tv_nsec - d2.tv_nsec) / 1000;
			    t_pow    += (d4.tv_sec - d3.tv_sec) * 1000000 + (d4.tv_nsec - d3.tv_nsec) / 1000;
			    t_total  += (d4.tv_sec - d0.tv_sec) * 1000000 + (d4.tv_nsec - d0.tv_nsec) / 1000;
			    if ((++n_sub % 25) == 0) {
			        fprintf(stdout, "diag-tx: %ld sub frames: audio(incl pace)=%ldus encode=%ldus"
			                        " send+radio=%ldus power=%ldus total=%ldus (40000us = real time)\n",
			                n_sub, t_audio / 25, t_encode / 25, t_send / 25, t_pow / 25, t_total / 25);
			        fflush(stdout);
			        t_audio = t_encode = t_send = t_pow = t_total = 0;
			    }
#endif
		    }
		    // Send EOT
		    m17_send_eot();
		    usleep(40000);
		    audio_mic_close();
		    // Go to receive
		    radio_receive();
        }

		// Full Duplex
		if( m_ptt == PTT_DP){
		    audio_mic_open();
		    audio_spk_open();
		    usleep(120000);// Buffer 6 sound blocks
		    // Go to transmit
		    radio_duplex();
	        // Send preamble
		    m17_send_carrier();
		    m17_send_preamble();
		    m17_send_preamble();
		    // Put the current position in the META field before announcing it
		    m17_tx_meta_from_gps(true);
		    // Send link setup
		    m17_send_link_setup_frame(m17_db_get_dst(), m17_db_get_src(), m_type, m_meta);
		    // A couple of blank codec frames before the voice (see m17_tx_lead_in)
		    m17_tx_lead_in();
		    // Everything the microphone captured while the radio was starting up
		    // is now stale: send live audio from here (see audio_input_flush).
		    audio_input_flush();
		    // Send sub frames of voice, 25 sub frames per sec
		    while((m_ptt == PTT_DP)&&M17_MODE_OK(gen)){
			    // Read Audio samples and encode using Codec 2
			    // Audio blocks are 20 ms but M17 chunks are 40 ms so 2 codec frames per m17 sub frame
			    audio_input(m_amicsamples, m_nsam);
			    codec2_encode(m_codec2, &payload[0], m_amicsamples);
			    // Read Audio samples and encode using Codec 2
			    audio_input(m_amicsamples, m_nsam);
			    codec2_encode(m_codec2, &payload[8], m_amicsamples);
			    // New superframe: the LICH is starting again, so this is the point
			    // at which the META field (and the position in it) may change.
			    if (m17_tx_lich_index() == 0) m17_tx_meta_from_gps(false);
			    // Send Sub frame
			    m17_send_stream_frame( payload );
			    // Apply a backend change made in the web UI (browser <-> Pi sound
			    // card) without waiting for the next transmission
			    if (audio_backend_pending()) audio_apply_pending();
			    // Receive audio
			    int len = radio_receive_samples( m_rsamples, N_SAMPLES );
			    m17_dsp_rx( m_rsamples, len);
			    radio_rssi_update();
		    }
		    // Send EOT
		    m17_send_eot();
		    usleep(40000);
		    audio_mic_close();
		    audio_spk_close();
		    // Go to receive
		    radio_receive();
        }

        // Simplex receive
		if( m_ptt == PTT_RX){
		    radio_receive();
		    audio_spk_open();
			while((m_ptt == PTT_RX)&&M17_MODE_OK(gen)){
			    // Re-open the audio paths if the web UI changed the backend
			    if (audio_backend_pending()) audio_apply_pending();
			    int len = radio_receive_samples( m_rsamples, N_SAMPLES );
			    m17_dsp_rx( m_rsamples, len );
			    // Throw input samples away
				radio_rssi_update();
			}
		    audio_spk_close();
		}

        // Simplex transmit carrier
		if( m_ptt == PTT_CA){
		    audio_spk_close();
		    audio_mic_close();
		    radio_transmit();
			while((m_ptt == PTT_CA)&&M17_MODE_OK(gen)){
			    m17_send_carrier();
			}
		}
	}
    // Close the system
    return arg;
}

void *m17_txrx_client_thread(void *arg){
    uint8_t payload[16];
    const int gen = m_mode_gen;                 // the mode this thread started for

    while(M17_MODE_OK(gen)){
        // Simplex transmit
		if( m_ptt == PTT_TX){
		    audio_mic_open();
		    // Send link setup
		    m17_tx_meta_from_gps(true);
		    m17_send_link_setup_frame_to_net(m17_db_get_dst(), m17_db_get_src(), m_type, m_meta);
		    // A couple of blank codec frames before the voice (see m17_tx_lead_in)
		    m17_tx_lead_in_to_net();
		    audio_input_flush();
		    // Send sub frames of voice, 25 sub frames per sec
		    while((m_ptt == PTT_TX)&&M17_MODE_OK(gen)){
			    // Read Audio samples and encode using Codec 2
			    // Audio blocks are 20 ms but M17 chunks are 40 ms so 2 codec frames per m17 sub frame
			    audio_input(m_amicsamples, m_nsam);
			    codec2_encode(m_codec2, &payload[0], m_amicsamples);
			    // Read Audio samples and encode using Codec 2
			    audio_input(m_amicsamples, m_nsam);
			    codec2_encode(m_codec2, &payload[8], m_amicsamples);
			    if (m17_tx_lich_index() == 0) m17_tx_meta_from_gps(false);
			    // Send Sub frame
			    m17_send_stream_frame_to_net( payload );
			    radio_power_update();
		    }
		    audio_mic_close();
        }

        // Simplex receive
		if( m_ptt == PTT_RX){
		    audio_spk_open();
			while((m_ptt == PTT_RX)&&M17_MODE_OK(gen)){
				usleep(1000);
			}
		}
	}
    return arg;
}
void *m17_txrx_audio_loopback(void *arg){
    uint8_t payload[16];
    const int gen = m_mode_gen;                 // the mode this thread started for
    while(M17_MODE_OK(gen)){
        if (audio_backend_pending()) audio_apply_pending();
        // Simplex transmit
		if( m_ptt == PTT_TX){
	        audio_input(m_amicsamples,  m_nsam);
		    codec2_encode(m_codec2, &payload[0], m_amicsamples);
		    m17_txrx_spkr_audio( payload );
        }else{
	        audio_input(m_amicsamples,  m_nsam);
        }
    }
    return arg;
}
//
// This is the thread function
//
void *m17_txrx_threads(void *arg){
    m_running   = true;
    m_net_state = STATE_RX;
    m_ptt = PTT_RX;

    audio_mic_open();
    audio_spk_open();

    while(m_running == true){
        // decide which sub thread should be running
    	if(m17_db_get_chan_type() == DRTOAS) m17_txrx_thread(NULL);
    	if(m17_db_get_chan_type() == ASTODN) m17_txrx_client_thread(NULL);
    	if(m17_db_get_chan_type() == DRTODN) m17_txrx_net_thread(NULL);
    	if(m17_db_get_chan_type() == ASTOAS) m17_txrx_audio_loopback(NULL);
        // A thread can return because the mode changed underneath it, so pause
        // briefly rather than spinning while the new mode starts.
        usleep(20000);
    }
    // Close the system
    audio_spk_close();
    audio_mic_close();
	return arg;
}
//
// If listen all is true then it decodes all addresses
// if false then only decodes own and broadcast address
//
void m17_txrx_listen_all( bool b ){
    m17_db_listen_all( b );
}
//
// Set the own address
//
void m17_txrx_set_src_add(const char *add){
	m17_db_set_src( add );
}
//
// Set the local gateway address
//
void m17_txrx_set_gate_add(const char *add){
	m17_db_set_gate( add );
}
//
// Set the address of station you are calling
//
void m17_txrx_set_dest_add(const char *add){
	m17_db_set_dst( add );
}
//
// Set address to call everyone
//
void m17_txrx_set_brd_add(void){
	m17_db_set_brd();
}

void m17_txrx_set_type(M17Type type){
    m_type = type;
}
void m17_txrx_set_meta(uint8_t *meta){
    memcpy(m_meta,meta,sizeof(uint8_t)*14);
}
//
// Go to transmit
//
// ---------------------------------------------------------------------------
// Hardware PTT
//
// radio_keyed() reports whether the physical switch on the Pi's header is
// pressed (see rpi_gpio.cpp).  Nothing used to poll it, which is why the
// hardware PTT never worked.  This thread polls it every 25 ms and drives the
// transmitter exactly as the button in the web interface does.  It only unkeys
// when the hardware was what keyed the station, so releasing the switch cannot
// cancel a transmission that was started from the browser.
// ---------------------------------------------------------------------------
static volatile bool m_hw_keyed;

bool m17_tx_hw_ptt(void) {
    return m_hw_keyed;
}

int m17_txrx_ptt_state(void) {
    switch (m_ptt) {
        case PTT_TX: return 1;
        case PTT_DP: return 2;
        case PTT_CA: return 3;
        default:     return 0;
    }
}

void *m17_txrx_ptt_thread(void *arg) {
    bool was = false;
    printf("m17web: hardware PTT monitor started\n");
    fflush(stdout);
    while (m_running == true) {
        bool now = radio_keyed();
        if (now != was) {
            was = now;
            if (now) {
                printf("m17web: hardware PTT pressed\n");
                fflush(stdout);
                m_hw_keyed = true;
                m17_txrx_ptt_on();
            } else {
                printf("m17web: hardware PTT released\n");
                fflush(stdout);
                if (m_hw_keyed) {
                    m_hw_keyed = false;
                    m17_txrx_ptt_off();
                }
            }
        }
        usleep(25000);              // 40 Hz is plenty for a switch
    }
    return arg;
}

void m17_txrx_ptt_on(void){
    m_ptt = PTT_TX;
    m17_db_set_ptt(false);
}
//
// Go to receive
//
void m17_txrx_ptt_off(void){
    m_ptt = PTT_RX;
    m17_db_set_ptt(false);
}
//
// Transmit a carrier
//
void m17_txrx_ptt_carrier(void){
    m_ptt = PTT_CA;
    m17_db_set_ptt(true);
}
void m17_txrx_ptt_duplex(void){
    m_ptt = PTT_DP;
}
void m17_txrx_enable(void){
	m_running = true;
}
// Close the system
void m17_txrx_disable(void){
	m_running = false;
}
// Init this module
void m17_txrx_init(void){
    // Optional default for the blank frames sent before the voice: "tx lead 6"
    // (or M17WEB_TX_LEAD=6) for a longer run-in on a slow microphone.
    const char *env = getenv("M17WEB_TX_LEAD");
    if (env && env[0]) m17_tx_set_lead_frames(atoi(env));
	m_codec2 = codec2_create(CODEC2_MODE_3200);
	if (m_codec2 == NULL) {
		// Silently continuing would transmit empty payloads that a receiver
		// rejects on the PRBS9 payload check - no voice, no obvious reason.
		fprintf(stderr, "m17web: codec2_create(CODEC2_MODE_3200) FAILED - no voice\n");
	}
	m_nsam   = codec2_samples_per_frame(m_codec2);
	printf("m17web: codec %s, %d samples and %d bits per frame\n",
	       m_codec2 ? "ready" : "MISSING", m_nsam, codec2_bits_per_frame(m_codec2));
	m17_db_listen_all( false );
}

