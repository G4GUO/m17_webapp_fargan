#ifndef M17DEFINES_H
#define M17DEFINES_H
#include <stdint.h>
#include <stdio.h>
#ifdef CODEC2_MOD
// Modified Codec2 (M17-Project/Codec2-mod) through the adapter in codec2mod/.
// 3200 bps only, which is what M17 uses.  See codec2mod/VENDORED.txt.
#include "codec2_shim.h"
#else
#include "codec2.h"
#endif

//#define __TEST__
//#define __TRACE__
// Over sample rate
#define N_OS 10
// Size of decimating filter in receiver
#define DF5_FN 71

typedef enum{RADIO_TYPE_PLUTO,RADIO_TYPE_LIME,RADIO_TYPE_SIM}RadioType;

// Simulated radio (radio_sim.cpp), selected with -r sim
const char *radio_name(void);
RadioType   radio_get_type(void);
double      radio_get_tx_gain(void);
double      radio_get_rx_gain(void);
double      radio_get_freq_correction(void);

// Length of preamble in samples
#define N_SAMPLES 1920
#define SRATE 48000

typedef uint16_t uint12_t;
typedef uint32_t uint24_t;
typedef uint64_t uint48_t;
/*
 * Type field definitions
 *
   0 	Packet/stream indicator, 0=packet, 1=stream
   1..2 	Data type indicator, 01 =data (D), 10 =voice (V), 11 =V+D, 00 =reserved
   3..4 	Encryption type, 00 = none, 01 =AES, 10 =scrambling, 11 =other/reserved
   5..6 	Encryption subtype (meaning of values depends on encryption type)
   7..10 	Channel Access Number (CAN)
   11..15 	Reserved (don’t care)
*/

typedef struct{
	uint8_t p_s;
	uint8_t dt;
	uint8_t et;
	uint8_t est;
	uint8_t can;
	uint8_t reserved;
}M17Type;

typedef struct{
	uint8_t votes;
	uint8_t type;
	uint8_t gerr;
	float variance;
	float best;                 // correlation sum of the winning pattern
	float second;               // and of the runner-up: the margin is what tells
	                            // a real sync from a coincidence
}M17Sync;

#define CCT_PACKET   0
#define CCT_STREAM   1
#define DATA_DATA    1
#define DATA_VOICE   2
#define DATA_DANDV   3
#define DATA_RES     0
#define ENC_NONE     0
#define ENC_AES      1
#define ENC_SCR      2
#define ENC_OTH      3
#define ENC_SUB      0
#define CAN_NUM      0
#define RES_RES      0
// Frame types
#define M17_EOT      5

#define FRAME_SYM_LENGTH 192
#define FRAME_BIT_LEN    384

//
// Circuit types
//
// AS = Analogue speaker / Mike
// AR = Analogue Radio FM
// DR = Digital Radio M17
// DN = Digital Network M17 over Internet
//
typedef enum{
	ASTOAS, // Audio Loopback
	ARTOAS, // FM  Radio
	DRTOAS, // M17 Radio
	ASTODN, // Audio Network
	ARTODN, // FM  Network
	DRTODN //  M17 Network
}CircuitType;

//
// Database info
//
typedef struct{
	char        rx_src_add[15];
	char        rx_gate_add[15];
	char        rx_dest_add[15];
	char        tx_src_add[15];
	char        tx_gate_add[15];
	char        tx_dest_add[15];
	uint48_t    rx_src;
	uint48_t    rx_dest;
	uint48_t    tx_src;
	uint48_t    tx_gate;
	uint48_t    tx_dest;
	uint64_t    rx_freq;
	uint64_t    tx_freq;
    bool        listen_flag;
    bool        in_frame;
    bool        ptt;
    uint32_t    g_errors;
    uint32_t    n_frames;
    CircuitType chan_type;
}M17_Dbase;

typedef struct{
	bool valid;                 // position is valid (fresh fix)
	double lat;                 // degrees, +N -S
	double lon;                 // degrees, +E -W
	int16_t alt;                // metres above mean sea level (M17 META is metric)
	uint8_t hour;
	uint8_t min;
	uint8_t sec;
	uint8_t day, month;
	uint16_t year;
	uint16_t course;            // degrees true
	uint16_t speed;             // (unused, kept for the old encoding)
	double speed_kmh;           // km/h from RMC/VTG
	bool have_velocity;         // speed and course are meaningful
	float hdop;                 // horizontal dilution of precision
	uint8_t validity;           // GNSS_VALID_* bit mask for the META field
	uint8_t station_type;       // GNSS_STATION_* for the META field
	uint8_t nsats;              // satellites used
	uint32_t fix_ms;            // CLOCK_MONOTONIC ms of the last fix
	uint32_t object;
}GpsMsg;

typedef struct{
	float re;
	float im;
}fcmplx;

typedef struct {
	int16_t re;
	int16_t im;
}scmplx;

// MMI
void mmi_parse(char *cmd);
void mmi_load_file(const char *name);

// Lime specific
int  lime_main(void);
int  lime_open(void);
void lime_got_to_duplex(void);
void lime_got_to_transmit(void);
void lime_got_to_receive(void);
void lime_set_tx_freq(uint64_t freq);
void lime_set_rx_freq(uint64_t freq);
void lime_close(void);
int  lime_transmit_samples( int16_t *in, int len);
int  lime_receive_samples(int16_t *samples, int len);
// read the state of the ptt
bool lime_read_ptt(void);
// set the ptt to transmit
void lime_ptt_tx(void);
// set the ptt to receive
void lime_ptt_rx(void);
// Transmitter level
void lime_set_tx_gain(float gain);
// Receiver gain
void lime_set_rx_gain(double gain);
double lime_get_rx_gain(void);
uint32_t lime_read_rssi(void);

// adalm-pluto

typedef enum{
	AGC_MANUAL, AGC_SLOW, AGC_FAST, AGC_HYBRID
}AgcType;

int  pluto_open( uint32_t block_size );
void pluto_close(void);
void pluto_set_ip_address(const char *add);
int  pluto_start_tx_stream(void);
void pluto_stop_tx_stream(void);
int  pluto_start_rx_stream(void);
void pluto_stop_rx_stream(void);
void pluto_set_tx_level(double level);
void pluto_set_rx_level(double level);
void pluto_apply_rx_level(void);
void pluto_agc_type(AgcType type);
void pluto_set_tx_freq(long long int freq);
void pluto_set_rx_freq(long long int freq);
void pluto_get_tx_freq(long long int *freq);
void pluto_get_rx_freq(long long int *freq);
void pluto_set_tx_sample_rate( long int sr);
void pluto_set_rx_sample_rate( long int sr);
void pluto_configure_x8_int_dec(long long int  sr);
void pluto_read_rssi_value(long long int *rssi);

uint32_t pluto_rx_samples( scmplx *s );
void pluto_tx_samples( scmplx *s, int len);
void pluto_tx_flush(void);          // drop the transmit accumulator (start of an over)
long pluto_tx_push_errors(void);    // sample buffers the radio refused

void pluto_transmit(void);
void pluto_receive(void);

//
// Generic radio interface
//
int  radio_open(RadioType type);
void radio_close(void);
void radio_transmit(void);
void radio_receive(void);
void radio_duplex(void);
bool radio_keyed(void);
void radio_set_tx_frequency(uint64_t freq);
void radio_set_rx_frequency(uint64_t freq);
int  radio_receive_samples( scmplx *s, uint32_t n);
int  radio_transmit_samples( scmplx *s, uint32_t n);
void radio_silence(int ms);
void radio_set_tx_gain(float gain);
void radio_set_rx_gain(float gain);
void radio_set_afc_on(void);
void radio_set_afc_off(void);
void radio_set_freq_correction_factor( double fac);
void radio_afc(float mean);
float radio_get_afc_delta(void);
bool radio_get_afc_status(void);
void radio_rssi_update(void);
void radio_power_update(void);
int  radio_get_oversample(void);

void m17_dsp_process_samples(short *buffer, int samplesRead);
void m17_dsp_build_rrc_filter(float *filter, float rolloff, int ntaps, int samples_per_symbol);
void m17_dsp_build_lpf_filter(float *filter, float bw, int ntaps);
void m17_dsp_float_to_short(float *in, int16_t *out, int len);

void m17_dsp_init(void);
void m17_dsp_enable_bs(void);
void m17_dsp_disable_bs(void);
void m17_dsp_set_filter_gain(float *filter, float gain, int stride, int ntaps);
void m17_dsp_rx(scmplx *in, int len);
void m17_dsp_display_coffs(float *in, int len);
int  m17_dsp_decimating_filter(float *in, float *out, float *coffs, int stride, int flen, int len);
float  *m17_dsp_update_buffer( float *in, int flen, int blen);
fcmplx *m17_dsp_update_buffer( fcmplx *in, int flen, int blen);
scmplx *m17_dsp_update_buffer( scmplx *in, int flen, int blen);

//
// De-map the symbol into 2 soft bits
// value is the 'probability' of receiving a 1
// 1.0 = 100%
// 0.5 = 50% (puncture value)
// 0   = 0%
//

void m17_dsp_demap_symbol(float in, float mag, float *out);
void m17_demap_set_llr(int on);
int  m17_demap_llr_mode(void);
void m17_dsp_demap_frame(float *in, float *out);

int  audio_main(void);
void audio_output(int16_t *s, int len);
void audio_input( int16_t *s, int len);
void audio_input_flush(void);
void audio_output_flush(void);
void audio_mic_open(void);
void audio_mic_close(void);
void audio_spk_open(void);
void audio_spk_close(void);
//
// Puncture / De puncture routines
//
int m17_punc_p1(uint8_t *in, uint8_t *out, int len);
int m17_punc_p2(uint8_t *in, uint8_t *out, int len);
int m17_punc_p3(uint8_t *in, uint8_t *out, int len);
int m17_de_punc_p1(float *in, float *out, int len);
int m17_de_punc_p2(float *in, float *out, int len);
int m17_de_punc_p3(float *in, float *out, int len);

//
// bit packing etc
// len is the input length
//
uint48_t m17_encode_call(const char *call);
char *m17_decode_call(uint48_t word, char *call);

int pack_8_to_1(uint8_t *in, uint8_t *out, int len);
int pack_1_to_2(uint8_t *in, uint8_t *out, int len);
int pack_1_to_8(uint8_t *in, uint8_t *out, int len);
int pack_48_to_8(uint64_t in, uint8_t *out);
int pack_24_to_8(uint24_t in, uint8_t *out);
int pack_24_to_2(uint24_t in, uint8_t *out);
int pack_24_to_1(uint24_t in, uint8_t *out);
int pack_16_to_8(uint16_t in, uint8_t *out);
int pack_16_to_2(uint16_t in, uint8_t *out);
int pack_12_to_8_x4x6(uint12_t *in, uint8_t *out);
uint48_t pack_8_to_48(uint8_t *in);
uint24_t pack_8_to_24(uint8_t *in );
uint16_t pack_8_to_16(uint8_t *in );
int pack_8_to_12_x4(uint8_t *in, uint12_t *out);
void prbs9(void);

uint24_t hard_decode_24_bits(float *in);

int pack_8_to_8(uint8_t *in, uint8_t *out, int len);
// Pack Type array into type field
uint16_t m17_pack_type(M17Type type);
M17Type m17_upack_type(uint16_t word);

void test_encode(void);

// Convolutional code
void m17_init_conv(void);
int m17_conv_encode_1(uint8_t *in, uint8_t *out, int len);
int m17_conv_encode_8(uint8_t *in, uint8_t *out, int len);
int m17_viterbi_decode(float *in, uint8_t *out, int len);

//
// Interleaver / Deinterleaver
//
void m17_interleave( uint8_t *in, uint8_t *out, int len);
void m17_de_interleave( float *in, float *out, int len);

//
// Golay encoder / decoder
//
uint24_t m17_golay_encode(uint12_t data);
int      m_17_golay_decode(uint24_t word, uint12_t &odata);
void m17_golay_init(void);

//
// CRC calculation
//
uint16_t m17_crc_array_encode( uint8_t *in, int len );
void m17_crc_init(void);
void m17_test_crc(void);

//
// de-Correlator
//
void m17_de_correlate_8( uint8_t *in, int len);
void m17_de_correlate_1( uint8_t *in, uint8_t *out, int len);
void m17_de_correlate_1( float *in, float *out, int len);
void m17_init_de_correlate(void);

//
// Tx frames
//
void m17_send_preamble(void);
// LICH / META during transmit (the GNSS position lives in the META field)
void m17_update_meta(uint8_t *meta);
int  m17_tx_lich_index(void);
bool m17_tx_gnss(void);
bool m17_tx_gnss_enabled(void);
void m17_tx_set_gnss_enabled(bool on);

void m17_send_link_setup_frame(uint48_t dest, uint48_t src, M17Type type, uint8_t *meta);
void m17_send_noise_preamble(void);
void m17_send_link_setup_frame_to_net(uint48_t dest, uint48_t src, M17Type type, uint8_t *meta);
void m17_send_link_setup_frame_fm_net(uint8_t *net);
void m17_send_stream_frame( uint8_t *payload);
void m17_send_stream_frame_fm_net( uint8_t *net);
void m17_send_stream_frame_to_net( uint8_t *payload );
void m17_send_bert_frame( void );
void m17_send_eot(void);
// Blank carrier
void m17_send_carrier(void);
void m17_fmt_init(void);

// tx modulator
void m17_mod_tx(int16_t *s, int len);
// initialise
void m17_mod_init(void);
// Send stream of dibits
void m17_mod_dibits(uint8_t *dibits, int len);
void m17_mod_flush(void);
// Send carrier
void m17_mod_carrier(void);

// m17_tests
void m17_send_test_frames( CODEC2 *codec, uint8_t *meta, int n_sams);

// rx frame
void m17_rx_sym(float sym);
void m17_rx_symbols(float *sym, int len);
void m17_rx_lost(void);
void m17_rx_init(void);
bool m17_rx_lock(void);

// rx parser
void m17_rx_parse(float *s, uint8_t type);
extern int m17_rx_type_count[6];
void m17_rx_parse_los(void);
void m17_rx_parse_aos(void);

// rx sync
int m17_rx_sync_samples( float *in, float *out, int len);
void m17_rx_sync_init(void);
int m17_sync_check(float *vect, int &type, int &e );

// Tx RX routines
void m17_txrx_init(void);
void *m17_txrx_threads(void *arg);

void m17_txrx_set_src_add(const char *add);
void m17_txrx_set_gate_add(const char *add);
void m17_txrx_set_dest_add(const char *add);
void m17_txrx_set_brd_add(void);
void m17_txrx_set_type(M17Type type);
void m17_txrx_set_meta(uint8_t *meta);
void m17_txrx_spkr_audio( uint8_t *data );

void m17_txrx_mode_changed(void);   // the mode changed: stand the running thread down
int  m17_txrx_mode_generation(void);
void m17_txrx_ptt_on(void);
void m17_txrx_ptt_off(void);
void m17_txrx_ptt_duplex(void);
void m17_txrx_enable(void);
void m17_txrx_disable(void);
void m17_txrx_listen_all( bool b );
// Send a carrier
void m17_txrx_ptt_carrier(void);
// Blank (silence) codec frames transmitted before the voice at key-down, in
// codec frames: 2 frames = one M17 sub frame = 40 ms.  Hides the receiver's
// acquisition and the local microphone's start-up, which otherwise appear as
// transmit dropouts.  Default 4 (80 ms), 0 disables.
int  m17_tx_lead_frames(void);
void m17_tx_set_lead_frames(int frames);

// PRBS9
void m17_prbs9_tx_load(uint8_t *out, int len);
void m17_prbs9_tx_reset(void);
void m17_prbs9_rx_reset(void);
int  m17_prbs9_rx_check(uint8_t bit);
void m17_prbs9_init(void);

// Equaliser
void eq_reset(void);
void eq_restart(void);
float eq_train_known(float *in, float train);
float eq_train_unknown( float *in );
void eq_open(void);
//
// Database routines
//

const M17_Dbase *m17_get_db( void );
void m17_db_set_chan_type(CircuitType type);
CircuitType m17_db_get_chan_type(void);
void m17_db_set_ptt(bool ptt);
bool m17_db_get_ptt(void);

void m17_db_set_src( const char *add );
void m17_db_set_gate( const char *add );
void m17_db_set_dst( const char *add );
void m17_db_set_rx_src( uint48_t src );
void m17_db_set_rx_gate( uint48_t dest );
void m17_db_set_rx_dst( uint48_t dest );
void m17_db_set_tx_freq(uint64_t freq);
void m17_db_set_rx_freq(uint64_t freq);

void     m17_db_set_brd( void );
uint48_t m17_db_get_src( void );
uint48_t m17_db_get_gate( void );
uint48_t m17_db_get_dst( void );
void     m17_db_listen_all( bool flag );
bool     m17_db_is_for_me(uint48_t add );
// Aquisition of signal
void m17_aos(void);
// Loss of signal
void m17_los(void);
// Errors detected in stream frame
void m17_db_golay_errors( uint16_t e );
// Sequence number of stream frame
void m17_db_stream_seq_number( uint16_t n);
// Are we receiving a frame ?
bool m17_db_in_frame(void);
// State of the PTT
void m17_ptt(bool ptt);

// PTT access via gpio
// Hardware PTT: the switches on the Raspberry Pi header (see rpi_gpio.cpp)
bool rpi_gpio_ok(void);           // both GPIO lines are claimed
bool rpi_gpio_pressed(void);      // last reading of the PTT input
int  rpi_gpio_out_pin(void);      // the keying output (BCM number)
int  rpi_gpio_in_pin(void);       // the PTT input (BCM number)
const char *rpi_gpio_error(void); // why it is unusable, blank when fine
void *m17_txrx_ptt_thread(void *arg);   // polls it and drives the transmitter
bool m17_tx_hw_ptt(void);         // the hardware is what keyed the station
int  m17_txrx_ptt_state(void);    // 0 RX, 1 TX, 2 duplex, 3 carrier

void rpi_tx( void );
void rpi_rx( void );
bool rpi_read_ptt(void);
int rpi_gpio_open(void);
void rpi_gpio_close(void);

// Presentation layer: web UI (webui.cpp) or ncurses console (ui_console.cpp).
// See ui.h for the whole interface, including ui_command_loop().
#include "ui.h"

// Audio routing (browser / PulseAudio / file)
#include "audio.h"

// Network
int  m17_net_open(void);
void m17_net_close(void);
bool m17_net_connected(void);
void m17_net_ref_address(char *name);
char m17_net_ref_module(void);

// Name = Reflector name
// IP_ADDR = Address of reflector
// from = address of client station
// mod = Reflector module
void m17_net_connect_to_reflector(const char *name, uint48_t from, const char mod);
void m17_net_disconnect_from_reflector(void);
bool m17_net_new_rx_data(uint16_t frame_id, uint8_t *lich, uint16_t fn, uint8_t *pld);
int udp_send(uint8_t *b, int len );

//
// Buffer handling
//
uint8_t *buff_alloc(void);
int buff_size(void);
void buff_rel(uint8_t *b);
void buff_post( uint8_t *b);
uint8_t *buff_get(void);
void buff_open(void);
void buff_close(void);

// GPS / M17 META GNSS position data (specification 2.0.9, encryption subtype 01)
#define GNSS_VALID_POSITION  0x8   // latitude/longitude valid
#define GNSS_VALID_ALTITUDE  0x4   // altitude valid
#define GNSS_VALID_VELOCITY  0x2   // bearing and speed valid
#define GNSS_VALID_RADIUS    0x1   // radius (HDOP-derived) valid

#define GNSS_SOURCE_M17_CLIENT 0   // Data Source field: 0 = M17 client
#define GNSS_STATION_FIXED     0
#define GNSS_STATION_MOBILE    1
#define GNSS_STATION_HANDHELD  2
#define GNSS_STATION_OTHER     15

// META content selected by the stream TYPE "encryption subtype" bits when the
// encryption type is none
#define META_TEXT           0
#define META_GNSS           1
#define META_EXT_CALLSIGN   2

// GPS
void gps_decode(uint8_t *b, GpsMsg *msg);
void gps_encode(uint8_t *b, GpsMsg *msg);
int gps_open(void);
void gps_close(void);
void gps_set_device(const char *dev, int baud);   // "" baud 0 = auto-detect
const char *gps_device(void);
int  gps_baud(void);
const char *gps_status(void);                     // last open/reopen result
bool gps_fix_fresh(int max_age_ms);               // valid fix within max_age_ms
uint32_t gps_fix_age_ms(void);                    // age of the last fix, ms
bool gps_rx_position(GpsMsg *out, char *src, int n);  // last position heard from another station
double gps_bearing(double lat1, double lon1, double lat2, double lon2);   // degrees true
double gps_distance_km(double lat1, double lon1, double lat2, double lon2);

// Position log (CSV)
void gps_csv_set(bool on, int seconds);           // seconds 0 = keep the current interval
bool gps_csv_on(void);
int  gps_csv_rows(void);
int  gps_csv_interval(void);
const char *gps_csv_path(void);
uint32_t gps_rx_age_ms(void);
double gps_lat(void);
double gps_lon(void);
int  nmea_decimal(const char *in, int n);         // used while parsing
void gps_maidenhead(char *out, int len);          // locator for the current fix
void gps_maidenhead_of(double lat, double lon, char *out, int len);
void gps_status_text(char *out, int len);         // one line for the log
void gps_format_position(double lat, double lon, char *out, int len);
extern GpsMsg m_gps;

#ifdef __TEST__
// test routines
void m17_test_bb(uint8_t *dibit);
void m17_disp_float_test(float *in, int len);
void m17_test_init(void);
#endif
#endif
