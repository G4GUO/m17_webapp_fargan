#include <math.h>
#include <memory.h>
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include "m17defines.h"
#include "m17_ax25.h"

// Receive-path tracing: build with DIAG=1 (makefile) to enable.
#ifdef M17WEB_DIAG
static void diag(const char *fmt, ...) {
	static int n = 0;
	if (n++ > 400) return;
	va_list ap; va_start(ap, fmt);
	fprintf(stdout, "diag-rx: "); vfprintf(stdout, fmt, ap); fprintf(stdout, "\n");
	va_end(ap);
	fflush(stdout);
}
#else
#define diag(...) do { } while (0)
#endif

static uint8_t m_lsf[2][30];
static uint8_t m_packet[800];
static int     m_packet_idx;
static uint16_t m_frame_id = 0xDEAD;

void generate_new_frame_id(void){
	m_frame_id = rand()&0xFFFF;
}
void copy_lich(void){
	memcpy(m_lsf[1],m_lsf[0],30);
}
void valid_packet_received(uint8_t *packet, int len){
	// AX.25 over M17: the payload is a type byte followed by the frame.  The
	// bridge strips the type byte and hands it to the KISS host; with packet
	// mode off it does nothing at all.
	m17_ax25_rx_packet( packet, len );
}

// The position a station last sent us: M17 puts a GNSS position in the LSF
// META field when the TYPE encryption subtype bits are 0b01 (and encryption is
// off). Kept here so the web interface can show who is where.
static GpsMsg   m_rx_gps;
static uint32_t m_rx_gps_ms;
static uint32_t m_rx_gps_logged;
static char     m_rx_gps_src[16] = "";

static uint32_t rx_now_ms(void){
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

// out may be NULL, src may be NULL.  Returns false if nothing was heard in the
// last minute.
bool gps_rx_position(GpsMsg *out, char *src, int n){
    if (!m_rx_gps_ms || (rx_now_ms() - m_rx_gps_ms) > 60000) return false;
    if (out) *out = m_rx_gps;
    if (src && n > 0) snprintf(src, n, "%s", m_rx_gps_src);
    return true;
}

uint32_t gps_rx_age_ms(void){
    if (!m_rx_gps_ms) return 0;
    return rx_now_ms() - m_rx_gps_ms;
}

static void valid_lsf_received(uint48_t src_add, uint48_t dst_add, uint8_t *meta, M17Type type){
	m17_db_set_rx_src( src_add );
	m17_db_set_rx_dst( dst_add );
	// A station announcing where it is. The LSF repeats every superframe (240 ms),
	// so the log line is throttled.
	if (type.et == 0 && type.est == META_GNSS) {
	    GpsMsg g;
	    memset(&g, 0, sizeof(g));
	    gps_decode(meta, &g);
	    if (g.valid) {
	        char call[16];
	        m17_decode_call(src_add, call);
	        memset(m_rx_gps_src, 0, sizeof(m_rx_gps_src));
	        snprintf(m_rx_gps_src, sizeof(m_rx_gps_src), "%s", call);
	        // m17_decode_call pads the callsign with spaces
	        for (int k = (int)strlen(m_rx_gps_src) - 1; k >= 0 && m_rx_gps_src[k] == ' '; k--)
	            m_rx_gps_src[k] = 0;
	        m_rx_gps = g;
	        m_rx_gps_ms = rx_now_ms();
	        if (m_rx_gps_ms - m_rx_gps_logged > 5000) {
	            m_rx_gps_logged = m_rx_gps_ms;
	            char grid[12], pos[96];
	            gps_maidenhead_of(g.lat, g.lon, grid, sizeof(grid));
	            snprintf(pos, sizeof(pos), "%.5f, %.5f", g.lat, g.lon);
	            printf("GNSS: %s at %s %s, %d m, %d deg, %.1f km/h\n",
	                   m_rx_gps_src, pos, grid, (int)g.alt, (int)g.course, g.speed_kmh);
	            fflush(stdout);
	        }
	    }
	}
    gui_update();
}

static void sound_data_received(uint8_t *data, int len){
	const M17_Dbase *db = m17_get_db();
	diag("stream audio: rx_dest=%llx for_me=%d", (unsigned long long)db->rx_dest, m17_db_is_for_me(db->rx_dest));
	if(m17_db_is_for_me( db->rx_dest ) == true ){
		m17_txrx_spkr_audio( &data[0] );
		m17_txrx_spkr_audio( &data[len/2]);
	}
}

void parse_packet( uint8_t *data, uint8_t eof, uint8_t fn){
    if(eof){
    	// FN is the number of valid bytes in the frame
        memcpy(&m_packet[m_packet_idx],data,fn);
        // Calculate the length of the packet
        m_packet_idx += fn;
        // Do a CRC on the packet
    	uint16_t crc = m17_crc_array_encode( m_packet, m_packet_idx );
    	if( crc == 0x0000 ){
    		// valid Packet of data
    		valid_packet_received(m_packet, m_packet_idx - 2);// Discard the CRC
    	}
        m_packet_idx = 0;
    }else{
        /* Chunk fn is 25 bytes at offset fn*25, so the next write goes at
         * (fn+1)*25.  This used to leave the index at fn*25, which made the
         * following chunk overwrite the one just stored: every frame longer than
         * one chunk failed its CRC and was dropped, while single chunk frames
         * (SABM, UA, RR) worked - so connections succeeded but no data passed. */
        memcpy(&m_packet[fn*25],data,25);
        m_packet_idx = (fn+1)*25;
    }
}
void parse_lsf( uint8_t *in ){
	uint16_t tw;
	uint48_t dst_add;
	uint48_t src_add;
	uint8_t meta[14];
	// Extract the destination address
	dst_add = pack_8_to_48(in);
	// Extract the src address
	src_add = pack_8_to_48(&in[6]);
	// Extract the type
	tw = pack_8_to_16( &in[12]);
	M17Type type = m17_upack_type(tw);
	// Extract the meta data
	memcpy(meta,&in[14],sizeof(uint8_t)*14);
	// Send this information to a higher layer
	valid_lsf_received( src_add, dst_add, meta, type);
	gui_save_dest_address(dst_add);
	gui_save_src_address(src_add);
}
void update_lich(uint8_t *in){
	// extract seq field
	int seq = in[5]>>5;// Mod 6 counter
	if( seq < 6 ){
		// refresh the data
		memcpy(&(m_lsf[0][seq*5]),in,sizeof(uint8_t)*5);
	    // Check the CRC
	    uint16_t crc = m17_crc_array_encode( m_lsf[0], 30 );
	    diag("lich seq=%d lsf_crc=0x%04x %s", seq, crc, crc == 0 ? "VALID" : "");

	    if( crc == 0x0000 ){
		    // valid LSF, save a good copy
	    	copy_lich();
		    parse_lsf(m_lsf[1]);
	    }
	}
}
void decode_link_frame(float *sb){
	float so[2][488];
	uint8_t bits[244];
	uint8_t bytes[31];
	m17_de_correlate_1( sb, sb, 368);
	m17_de_interleave( sb, so[0], 368);
	// de puncture
	m17_de_punc_p1(so[0], so[1], 488);
	// Viterbi
	m17_viterbi_decode( so[1], bits, 488);
	// Pack LSF
	pack_1_to_8(&bits[1], bytes, 240);// Discard 4 tail bits
	// Upstream tested m_packet (an unrelated packet buffer) here, so the LSF of a
	// link setup frame was never actually validated.  Check the frame we decoded.
	uint16_t lsf_crc = m17_crc_array_encode( bytes, 30 );
	diag("link setup lsf_crc=0x%04x %s", lsf_crc, lsf_crc == 0 ? "VALID" : "");
	if(lsf_crc != 0){
		char hex[3*30 + 1];
		int  o = 0;
		for(int i = 0; i < 30; i++)
			o += snprintf(&hex[o], sizeof(hex) - o, "%02x", bytes[i]);
		diag("lsf bytes: %s", hex);
	}
	if(lsf_crc == 0){
	    /* The standalone LSF is the frame that actually arrives intact - the
	     * chunked LICH copies rarely assemble into a valid 30 bytes.  The
	     * stream decoder gates every payload byte on m_lsf[1], which until now
	     * was only ever written by copy_lich(), so a good LSF frame was parsed
	     * and then discarded and the stream path waited forever on a stale
	     * buffer.  Store it: nothing else needs to know about the LICH. */
	    memcpy(m_lsf[1], bytes, 30);
	    parse_lsf(bytes);
	}
}
//
// Soft bits start at the payload (skip the 16 bit sync)
//
void decode_stream_frame(float *sb){
	float so[2][368];
	uint24_t gw[4];
	uint12_t w[4];
	uint8_t  lich[6];
	uint8_t  bits[148];
	uint8_t  pld[19];
	uint16_t fn;
	uint16_t e;

	m17_de_correlate_1( sb, sb,    368);
	m17_de_interleave(  sb, so[0], 368);
	// Extract Golay words
	gw[0] = hard_decode_24_bits(&so[0][0]);
	gw[1] = hard_decode_24_bits(&so[0][24]);
	gw[2] = hard_decode_24_bits(&so[0][48]);
	gw[3] = hard_decode_24_bits(&so[0][72]);
	// Error correct them
	e  = m_17_golay_decode( gw[0], w[0] );
	e += m_17_golay_decode( gw[1], w[1] );
	e += m_17_golay_decode( gw[2], w[2] );
	e += m_17_golay_decode( gw[3], w[3] );
	// Report number of errors
	m17_db_golay_errors( e );
#ifdef __TRACE__
	printf(" Golay errors %d ",e);
#endif
	// Pack into an 8 bit array 6 bytes long
	pack_12_to_8_x4x6(w, lich);
	// If CRC is correct it will save a good copy
	update_lich( lich );
	// Now do the convolutional bits
	// de puncture
	m17_de_punc_p2(&so[0][96], so[1], 296);
	// Viterbi
	m17_viterbi_decode( so[1], bits, 296);
	// Pack bits into byte array
	pack_1_to_8(&bits[1], pld, 144);// discard 4 tail bits ???????
	// Extract seq
    fn = pack_8_to_16( pld );
    m17_db_stream_seq_number(fn);
//     printf("FN %d \n",fn);
    // Only pass data to higher layer if a valid lich has been received
    uint16_t stored_crc = m17_crc_array_encode( m_lsf[1], 30 );
    diag("stream fn=%d stored_lsf_crc=0x%04x chan=%d", fn, stored_crc, (int)m17_db_get_chan_type());
    if(stored_crc == 0){
        // Point to start of data
        uint8_t *data = &pld[2];
        // A link setup frame tagged for the AX.25 bridge carries data, not
        // audio: hand the payload to it instead of the speaker or the network.
        if(m17_ax25_is_stream_tag(&m_lsf[1][14])){
            m17_ax25_rx_stream(data, 16);
            return;
        }
        if(m17_db_get_chan_type() == DRTODN ){
        	// Send to the network
     	    m17_net_new_rx_data( m_frame_id, m_lsf[1], fn, data);
        }
        if(m17_db_get_chan_type() == DRTOAS ){
 	        // Play M17 over speakers
 	        sound_data_received(data, 16);
 	    }
    }
}
void decode_packet_frame(float *sb){
	float so[2][420];
	uint8_t bits[240];
	uint8_t bytes[240];
	m17_de_correlate_1( sb, sb, 368);
	m17_de_interleave( sb, so[0], 368);
	// de puncture
	m17_de_punc_p3(so[0], so[1], 420);
	// Viterbi
	m17_viterbi_decode( so[1], bits, 420);
	/* 206 type 1 bits: 200 of chunk and 6 of metadata.  Reading 208 took two
	 * bits that belong to the flush bits, which shifted the metadata. */
	pack_1_to_8(&bits[1], bytes, 206);
	// Extract EOF file
	uint8_t eof = bytes[25]>>7;
	// Extract fn field
	uint8_t fn = (bytes[25]>>2)&0x1F;
	/* printf, not diag(): diag() has a 400 line budget per process and goes
	 * silent on a busy receiver, which is exactly when a lost chunk matters. */
	printf("m17web: packet frame eof=%d fn=%d\n", eof, fn);
	fflush(stdout);
	parse_packet( bytes, eof, fn);
}
void decode_bert_frame(float *sb){

}

//
// Parse an incoming frame of symbols
//
void m17_rx_parse(float *s, uint8_t type){
	float sb[384];// soft bits
	// Any frame heard means the channel is busy: a packet transmission waits
	// for it to go quiet (see m17_ax25_service).
	m17_ax25_note_activity();
	//float m_f_sb[384];// soft bits
#ifdef __TRACE__
    printf("Type: %d ",type);
#endif
	switch(type){
	case 0:
		// Preamble
		generate_new_frame_id();
		break;
	case 1:
		// Link
		m17_dsp_demap_frame(s, sb);
		decode_link_frame(sb);
		break;
	case 2:
		// Stream
		m17_dsp_demap_frame(s, sb);
		decode_stream_frame(sb);
		break;
	case 3:
		// Packet
		m17_dsp_demap_frame(s, sb);
		decode_packet_frame(sb);
		break;
	case 4:
		// Bert
		m17_dsp_demap_frame(s, sb);
		decode_bert_frame(sb);
		break;
	case 5:
		// EOT
		generate_new_frame_id();
		break;
	default:
		break;
	}
#ifdef __TRACE__
	printf("\n");
#endif
}
void m17_rx_parse_aos(void){
	generate_new_frame_id();
}
void m17_rx_parse_los(void){
	generate_new_frame_id();
}

