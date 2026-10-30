//
// radio.cpp
//
// Radio abstraction.  This is where you add new radios.
//
// Back ends are selected with the makefile:
//   HAVE_PLUTO     Pluto / Pluto+ over libiio
//   HAVE_LIME      LimeSDR over LimeSuite
//   HAVE_SIM       simulated radio (no hardware, TX looped back into RX)
//   HAVE_RPI_GPIO  Raspberry Pi GPIO PTT in/out
//
#include <math.h>
#include <memory.h>
#include <stdio.h>
#include "m17defines.h"

#ifdef HAVE_SIM
#include "radio_sim.h"
#endif

static bool m_afc;
static double m_cor_fac;
static float m_afc_delta;
static double m_ss;
static double m_pwr;
static double m_rx_gain = 0.7;
static RadioType m_radio_type;
static bool m_keyed;

#define NFIL 31

#ifdef HAVE_PLUTO
static scmplx m_rx_buff[N_SAMPLES+NFIL];
static int16_t m_dec_filter[NFIL];

static scmplx sub_filter(scmplx *in, int16_t *coffs ) {
	int32_t real, imag;
	scmplx sum;

    real = in[NFIL/2].re * coffs[NFIL/2];
    imag = in[NFIL/2].im * coffs[NFIL/2];

	for (int i = 0; i < NFIL/2; i++) {
	    real += coffs[i]*(in[i].re + in[(NFIL-1)-i].re);
	    imag += coffs[i]*(in[i].im + in[(NFIL-1)-i].im);
	}
	sum.re = real >> 15;
	sum.im = imag >> 15;
	return sum;
}
//
// Filter and decimate by 8 (the Pluto runs 8 times faster than the decoder
// baseband rate, see radio_get_oversample())
//
static void rx_decimate_filter(scmplx *in, scmplx *out){
    for( int i = 0; i < N_SAMPLES/8; i++){
    	out[i] = sub_filter(&in[i*8], m_dec_filter);
    }
}
//
// Filter used to decimate the receive samples.
//
void build_pluto_rx_dec_filter(void){
	float filter[NFIL];

	m17_dsp_build_lpf_filter( filter, 0.125f, NFIL);
	m17_dsp_set_filter_gain(filter, 0.9, 1, NFIL);
	m17_dsp_float_to_short(filter, m_dec_filter, NFIL);

}
#endif // HAVE_PLUTO

const char *radio_name(void){
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME )  return "lime";
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ) return "pluto";
#endif
#ifdef HAVE_SIM
	if( m_radio_type == RADIO_TYPE_SIM )   return "sim";
#endif
	return "none";
}

RadioType radio_get_type(void){ return m_radio_type; }

int radio_open(RadioType type){
	m_radio_type = type;

	m_cor_fac = 1.0;
	m_afc_delta = 0;
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME ) lime_open();
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ){
		if (pluto_open(N_SAMPLES) < 0) {
			// No Pluto (unplugged, or the USB network interface has no address):
			// keep running with the simulator so the web UI stays up and the
			// reason is visible, instead of dying.
			fprintf(stderr,
			        "m17web: Pluto not reachable - check the USB network interface\n"
			        "        (e.g. eth1 with 192.168.2.2/24, then the Pluto answers on 192.168.2.1)\n");
#ifdef HAVE_SIM
			fprintf(stderr, "m17web: falling back to the simulated radio (-r sim)\n");
			m_radio_type = RADIO_TYPE_SIM;
			sim_open();
#else
			fprintf(stderr, "m17web: no simulator compiled in, radio unusable\n");
#endif
			return -1;
		}
		uint32_t srate = 48000;
		pluto_set_rx_sample_rate(   srate*64 );
		pluto_set_tx_sample_rate(   srate*64 );
		pluto_configure_x8_int_dec( srate*8 );
		build_pluto_rx_dec_filter();
	}
#endif
#ifdef HAVE_SIM
	if( m_radio_type == RADIO_TYPE_SIM ) sim_open();
#endif
#ifdef HAVE_RPI_GPIO
	rpi_gpio_open();
#endif
	return 0;
}
void radio_close(void){
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME ) lime_close();
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ) pluto_close();
#endif
#ifdef HAVE_SIM
	if( m_radio_type == RADIO_TYPE_SIM ) sim_close();
#endif
#ifdef HAVE_RPI_GPIO
	rpi_gpio_close();
#endif
}
void radio_transmit(void){
	m_keyed = true;
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ) pluto_tx_flush();   // start the over clean
#endif
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME ){
	    lime_got_to_transmit();
	    // No lime_ptt_tx(): the Mini has no GPIO header.  The station's PTT
	    // line is driven by rpi_tx() below, whichever radio is fitted.
	}
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ){
	    pluto_stop_rx_stream();
	    pluto_start_tx_stream();
	}
#endif
#ifdef HAVE_RPI_GPIO
	rpi_tx();      // Use GPIO on RPi
#endif
	gui_tx();
}
void radio_receive(void){
	m_keyed = false;
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME ){
		lime_got_to_receive();
	}
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ){
	    pluto_stop_tx_stream();
	    pluto_start_rx_stream();
	    pluto_apply_rx_level();
	}
#endif
#ifdef HAVE_RPI_GPIO
	rpi_rx(); // Use GPIO on RPi
#endif
	gui_rx();
}
void radio_duplex(void){
	m_keyed = true;
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME ){
		lime_got_to_duplex();
	}
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ){
	    pluto_start_rx_stream();
	    pluto_apply_rx_level();
	    pluto_start_tx_stream();
	}
#endif
#ifdef HAVE_RPI_GPIO
	rpi_tx();      // Use GPIO on RPi
#endif
	gui_dp();
}

//
// Is the radio keyed by the (external) PTT input?
// Upstream this always called the Lime function, which meant a Pluto based
// station could never see its own PTT line.
//
bool radio_keyed(void){
#ifdef HAVE_SIM
	if( m_radio_type == RADIO_TYPE_SIM ) return sim_is_keyed();
#endif
#ifdef HAVE_LIME
	// The Pi's GPIO, not the Lime's.  Only the LimeSDR-USB has a GPIO header:
	// LMS_GPIORead on a Mini fails and leaves the readback at zero, which this
	// would report as permanently keyed.  PTT in is the same pin either way.
	if( m_radio_type == RADIO_TYPE_LIME ){
#ifdef HAVE_RPI_GPIO
		return rpi_read_ptt();
#endif
		return m_keyed;
	}
#endif
#ifdef HAVE_RPI_GPIO
	if( m_radio_type == RADIO_TYPE_PLUTO ) return rpi_read_ptt();  // Use GPIO on RPi
#endif
	return m_keyed;
}
void radio_set_tx_frequency(uint64_t freq){
	m17_db_set_tx_freq(freq);
	freq = freq * m_cor_fac;
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME )  lime_set_tx_freq(freq);
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ) pluto_set_tx_freq(freq);
#endif
}
void radio_set_rx_frequency(uint64_t freq){
	m17_db_set_rx_freq(freq);
	freq = freq * m_cor_fac;
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME )  lime_set_rx_freq(freq);
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ) pluto_set_rx_freq(freq);
#endif
}
void radio_set_tx_gain(float gain){
	m_pwr = gain;
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME ) lime_set_tx_gain(gain);
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ){
		gain = -(1.0 - gain)*89.0;
		if( gain > 0 ) gain = 0;
		if( gain < -89.0 ) gain = -89;
		pluto_set_tx_level(gain);
	}
#endif
#ifdef HAVE_SIM
	if( m_radio_type == RADIO_TYPE_SIM ) sim_set_tx_gain(gain);
#endif
}
void radio_set_rx_gain(float gain){
	m_rx_gain = gain;
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME ) lime_set_rx_gain(gain);
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ){
		/* RX gain is 0..73 dB of gain, not TX-style attenuation. */
		gain = gain * 73.0;
		if( gain < 0 ) gain = 0;
		if( gain > 73 ) gain = 73;
		pluto_set_rx_level(gain);
	}
#endif
#ifdef HAVE_SIM
	if( m_radio_type == RADIO_TYPE_SIM ) sim_set_rx_gain(gain);
#endif
}
double radio_get_tx_gain(void){ return m_pwr; }
double radio_get_rx_gain(void){ return m_rx_gain; }
double radio_get_freq_correction(void){ return m_cor_fac; }

void radio_set_afc_on(void){
    m_afc = true;
}
void radio_set_afc_off(void){
	m_afc_delta = 0;
    m_afc = false;
}
bool radio_get_afc_status(void){
    return m_afc;
}

int radio_receive_samples( scmplx *s, uint32_t n){
	int nr = 1920;
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME ){
		nr = lime_receive_samples((int16_t *)s, n);
	}
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ){
		// THe Pluto requires a high sample rate to work
		// but crashes if it's buffers are too big
		// So we do it in small chunks
		int op = 0;
		for( int i = 0; i < 8; i++){
			// Update buffer
			memcpy(m_rx_buff, &m_rx_buff[N_SAMPLES],sizeof(scmplx)*NFIL);
			nr = pluto_rx_samples( &m_rx_buff[NFIL] );
			// Filter and decimate the samples
			rx_decimate_filter(m_rx_buff, &s[op]);
			op += 240;
		}
	}
#endif
#ifdef HAVE_SIM
	if( m_radio_type == RADIO_TYPE_SIM ){
		nr = sim_receive_samples( s, n );
	}
#endif
	return nr;
}
/* Zero samples to the transmitter for a given number of milliseconds.  Used to
 * start and finish a data burst at zero amplitude: the modulator is constant
 * envelope, so keying straight into symbols and dropping the carrier straight
 * after them is a step change at each end. */
void radio_silence(int ms){
	static scmplx quiet[15360];
	int rate = 4800 * radio_get_oversample();
	int left = (rate / 1000) * ms;
	while( left > 0 ){
		int n = (left > (int)(sizeof quiet / sizeof quiet[0])) ? (int)(sizeof quiet / sizeof quiet[0]) : left;
		radio_transmit_samples( quiet, (uint32_t)n );
		left -= n;
	}
}
int radio_transmit_samples( scmplx *s, uint32_t n){
	int nr = 0;
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME ){
	    nr = lime_transmit_samples((int16_t *)s, n);
	}
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ){
	    pluto_tx_samples( s, n);
	}
#endif
#ifdef HAVE_SIM
	if( m_radio_type == RADIO_TYPE_SIM ){
	    sim_transmit_samples( s, n);
	    nr = (int)n;
	}
#endif
	return nr;
}
void radio_set_freq_correction_factor( double fac){
	if((fac > 0.9)&&(fac < 1.1)){
	    m_cor_fac = fac;
	}
}
//
// Automatic frequency control
//
void radio_afc(float mean){
	if((m_afc == true)&&(m17_db_in_frame()==true)){
		m_afc_delta -= mean * 0.1;
	}
}
float radio_get_afc_delta(void){
	if((m_afc == true)&&(m17_db_in_frame()==true)){
		return m_afc_delta;
	}else{
		m_afc_delta = 0;
		return 0;
	}
}
// return the required over sampling for the SDR (relative to the decoder
// baseband rate of 2 samples per symbol at 4800 symbols/s)

int radio_get_oversample(void){
#ifdef HAVE_LIME
	if( m_radio_type == RADIO_TYPE_LIME ){
	    return 10;
	}
#endif
#ifdef HAVE_PLUTO
	if( m_radio_type == RADIO_TYPE_PLUTO ){
	    return 80;
	}
#endif
#ifdef HAVE_SIM
	if( m_radio_type == RADIO_TYPE_SIM ){
	    // Same as the Pluto: the simulator decimates by 8 on receive
	    return 80;
	}
#endif
	return 10;
}

#define RSSI_DES 4000
#define RSSI_TOL 1500

void radio_rssi_update(void){

#ifdef HAVE_LIME
    if( m_radio_type == RADIO_TYPE_LIME ){
        uint32_t  rssi = lime_read_rssi();
        double gain = 1.0;
        gain = lime_get_rx_gain();

        if((rssi > (RSSI_DES-RSSI_TOL))&&(rssi < (RSSI_DES+RSSI_TOL))){
    	    //RSSI is OK
            if( rssi > RSSI_DES ) gain -= 0.0001;
            if( rssi < RSSI_DES ) gain += 0.0001;
        }else{
            // Need to rapid adjust the gain
            if( rssi > RSSI_DES ) gain -= 0.1;
            if( rssi < RSSI_DES ) gain += 0.1;
        }
        if( gain > 1.0 )      gain  = 1.0;
        if( gain < 0.1 )      gain  = 0.1;
        // Psuedo signal strength
        m_ss = 1.0 - gain;
	    lime_set_rx_gain(gain);
    }
#endif

#ifdef HAVE_PLUTO
    if( m_radio_type == RADIO_TYPE_PLUTO ){
    	long long int rssi;
    	pluto_read_rssi_value(&rssi);
    	// Max signal == 40
    	// Min signal == 125
    	double v = rssi;
    	v = v/130.0;
    	v = 1.0 - v;
    	m_ss = v;
    }
#endif
#ifdef HAVE_SIM
    if( m_radio_type == RADIO_TYPE_SIM ){
    	m_ss = sim_get_level();
    }
#endif
	if(m_ss > 1.0 ) m_ss = 1.0;
	if(m_ss < 0.0 ) m_ss = 0.0;
    gui_signal(m_ss);
}
void radio_power_update(void){
    gui_power(m_pwr);
}
