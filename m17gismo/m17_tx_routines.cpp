#include <stdio.h>
#include <math.h>
#include <memory.h>
#include "m17defines.h"

#define SYNC_LINK_SETUP 0x55F7
#define SYNC_STREAM     0xFF5D
#define SYNC_PACKET     0x75FF
#define SYNC_BERT       0xDF55

uint32_t m_tx_l; // length of dibits to be sent
uint8_t  m_tx_d[5000];// dibits
uint8_t  m_lich[30];
uint8_t  m_lich_count;
uint16_t m_fn;
// Network packets
uint8_t  m_net[54];
uint16_t m_net_sid;
uint16_t m_net_fn;

//
// add preamble as dibits
//
int m17_fmt_add_tx_preamble( uint8_t *dibits ){
	int idx = 0;
	for( int i = 0; i < 192/2; i++){
		dibits[idx++] = 0x01;
		dibits[idx++] = 0x03;
	}
	return idx;
}
// Index of the next 5 byte LICH chunk. 0 means a new superframe is starting,
// which is the only point at which the META field may change.
int m17_tx_lich_index(void){
	return m_lich_count;
}

// Replace the META field of the current LICH and refresh its CRC. Used to carry
// a fresh GNSS position during a transmission: the six stream frames of a
// superframe carry the whole LSF, so rewriting it here changes what the next six
// frames announce (and the CRC changes with it).
void m17_update_meta(uint8_t *meta){
	pack_8_to_8(meta, &m_lich[14], 14);
	uint16_t crc = m17_crc_array_encode(m_lich, 28);
	m_lich[28] = (crc>>8);
	m_lich[29] = (crc&0xFF);
}

void m17_send_dibit_array(void){
	m17_mod_dibits(m_tx_d, m_tx_l);
	m_tx_l = 0;
}

int build_lich( uint48_t dest, uint48_t src, M17Type type, uint8_t *meta){
	int idx = 0;
	// Add Source address (addresses need padding to 9 octets
	idx += pack_48_to_8(dest, &m_lich[idx]);
	// Add destination address (addresses need padding to 9 octets
	idx += pack_48_to_8(src, &m_lich[idx]);
	// Add the type field
	uint16_t word = m17_pack_type(type);
	idx += pack_16_to_8(word, &m_lich[idx]);
	// Add the meta
	idx += pack_8_to_8(meta, &m_lich[idx],14);
	// Add the CRC
	uint16_t crc = m17_crc_array_encode( m_lich, idx );
	m_lich[idx++] = (crc>>8);
	m_lich[idx++] = (crc&0xFF);
    return idx;
}
/* The _net functions translate between M17 air frames and M17 over IP datagrams
 * (reflector link).  See docs/m17-ip-framing.md for the byte layout and for the
 * 54 vs 56 byte difference against the published IP specification. */
void build_lich_to_net(uint48_t dest, uint48_t src, M17Type type, uint8_t *meta){
	// Format a net frame, no sending
	m_net[0] = 'M';
	m_net[1] = '1';
	m_net[2] = '7';
	m_net[3] = ' ';
	m_net_sid = rand()&0xFFFF;
	m_net[4] = (m_net_sid>>8);
	m_net[5] = (m_net_sid&0xFF);
	pack_48_to_8(dest,&m_net[6]);
	pack_48_to_8(src,&m_net[12]);
	uint16_t word = m17_pack_type(type);
	pack_16_to_8(word, &m_net[18]);
	// Add the meta
	pack_8_to_8(meta, &m_net[20],14);

}

int build_lich_from_net( uint8_t *net){
    uint16_t type;
    uint48_t destin;
    uint48_t source;
    uint8_t *meta;
    M17Type tp;
    // Skip magic
    destin = pack_8_to_48(&net[6]);
    source = pack_8_to_48(&net[12]);
    type   = pack_8_to_16(&net[18]);
    meta   = &net[20];// 14 bytes
    tp     = m17_upack_type(type);
	int n = build_lich( destin, source, tp, meta);
	return n;
}
//
// Send an initial link setup frame
//
// LSF_SYNC + LSF
//
#ifdef M17WEB_DIAG
static uint8_t m_diag_coded[512];
#endif
int m17_fmt_add_link_setup_frame(uint8_t *dibits, uint48_t dest, uint48_t src, M17Type type, uint8_t *meta){
	/* 240 data bits plus 4 flush encode to 488 bits, and the puncturer writes
	 * 368 into the second row while still reading the tail of the first.  The
	 * buffers used to be 388 bytes, so the coded stream overflowed into the row
	 * the puncturer was writing - it destroyed its own input from index 388
	 * onwards, which is the last six bytes of the frame: META and the CRC. */
	uint8_t tx_bit[2][512];
	int idx = 0;
	int len = 0;

	// Reset the counters for subframes
	m_lich_count = 0;
	m_fn         = 0;

	len = build_lich( dest, src, type, meta );
	// Apply the convolutional encoding
	len = m17_conv_encode_8( m_lich, tx_bit[0], len);
#ifdef M17WEB_DIAG
	memcpy(m_diag_coded, tx_bit[0], 488);
#endif
	// Puncture the code
	len = m17_punc_p1( tx_bit[0], tx_bit[1], len);
	// Interleave
	m17_interleave( tx_bit[1], tx_bit[0], len);
	// de-correlate
	m17_de_correlate_1( tx_bit[0], tx_bit[1], len);
#ifdef M17WEB_DIAG
	{	/* Hard-bit round trip of the LSF: bypasses the demapper entirely, so a
		 * failure here is the coding chain and a success here with a failing
		 * demodulated decode points at the soft values. */
		static float fs[512], f2[512];
		static uint8_t fb[512], fbytes[40];
		int fi;
		uint16_t fcrc;
		for (fi = 0; fi < 368; fi++) fs[fi] = tx_bit[1][fi] ? 1.0f : -1.0f;
		m17_de_correlate_1(fs, fs, 368);
		m17_de_interleave(fs, f2, 368);
		m17_de_punc_p1(f2, fs, 488);
		{	/* Which stage loses the tail?  Compare the de-punctured stream with
			 * the encoder's output at every position the pattern keeps. */
			int mism = 0, kept = 0, first = -1, last = -1;
			for (int k = 0; k < 488; k++) {
				/* de_punc writes 0.0 for a punctured position */
				if (fs[k] == 0.0f) continue;
				kept++;
				if ((fs[k] > 0) != (m_diag_coded[k] != 0)) {
					mism++;
					if (first < 0) first = k;
					last = k;
				}
			}
			printf("m17web: coded check kept=%d mismatched=%d first=%d last=%d\n",
			       kept, mism, first, last);
			fflush(stdout);
		}
		m17_viterbi_decode(fs, fb, 488);
		pack_1_to_8(&fb[1], fbytes, 240);
		fcrc = m17_crc_array_encode(fbytes, 30);
		printf("m17web: hardbit lsf_crc=0x%04x ", fcrc);
		for (fi = 0; fi < 30; fi++) printf("%02x", fbytes[fi]);
		printf("\n");
		fflush(stdout);
	}
#endif
	// Pack the frame into Dibits
	// Add the LSF_SYNC first
	idx = pack_16_to_2(SYNC_LINK_SETUP, dibits);
	// The add the payload
	idx += pack_1_to_2(tx_bit[1], &dibits[idx], len);
	// We now have a packed dibit array for transmitting
	return idx;
}
//
// Add a link setup from a net packet
//
int m17_fmt_add_link_setup_frame_fm_net(uint8_t *dibits, uint8_t *net){
    uint16_t type;
    uint48_t destin;
    uint48_t source;
    uint8_t *meta;
    M17Type tp;
    // Skip magic
    destin = pack_8_to_48(&net[6]);
    source = pack_8_to_48(&net[12]);
    type   = pack_8_to_16(&net[18]);
    meta   = &net[20];// 14 bytes

    tp     = m17_upack_type(type);

    int n = m17_fmt_add_link_setup_frame(dibits, destin, source, tp, meta);
    return n;
}
//
// Send Sub frame
// FRAME_SYNC + FRAME_DATA
// Payload is bytes bits
//
int m17_fmt_add_stream_frame(uint8_t *dibits, uint8_t *payload ){
	uint8_t tmp[80];
	uint8_t txb[2][388];
	uint12_t dw[4];
	uint24_t dpw;

	int idx;
    // Add 5 bytes of the LICH
	idx = pack_8_to_8(&m_lich[m_lich_count*5],tmp, 5);
	// Combine the 3 bits of the LIC field with the 5 reserved bits
	// Add 3 bits of the LICH counter
    tmp[idx++] = (m_lich_count&0x07)<<5;
    // Update the LICH counter
	m_lich_count = (m_lich_count+1)%6;
    // Partition into 12 bit words
	pack_8_to_12_x4(tmp, dw);
    // Golay Encode the 4 words and pack into output bitstream
	int len = 0;
	for( int i = 0; i < 4; i++){
		dpw  = m17_golay_encode(dw[i]);
		len += pack_24_to_1( dpw, &txb[0][len]);
	}
	// Save the location where the payload and FN data bits start
	int fn_start = len;
    // Add the 16 bit frame number
	idx = pack_16_to_8(m_fn, tmp);
	// increment the frame number
	m_fn = (m_fn+1)&0xFFFF;
	// Add the payload data
	idx += pack_8_to_8( payload, &tmp[idx], 16);
	// Convolutionally encode the frame number and payload
	len = m17_conv_encode_8(tmp, &txb[0][fn_start], idx);
	// Puncture the code and add them to the bitstream
	len = m17_punc_p2( &txb[0][fn_start], &txb[0][fn_start], len);
	// Interleave the whole bitstream
	m17_interleave( txb[0], txb[1], len + fn_start);
	// Decorrelate
	m17_de_correlate_1( txb[1], txb[0], len + fn_start);
	// Add Stream frame sync word
	idx = pack_16_to_2(SYNC_STREAM, dibits );
	// Add the rest as dibits
	idx += pack_1_to_2(txb[0], &dibits[idx], len + fn_start);
	//printf("Sub %d\n",idx);
	return idx;
}
int m17_fmt_add_stream_frame_fm_net(uint8_t *dibits, uint8_t *net ){
    uint8_t *pld;
    pld    = &net[36];// Voice data
    int n  = m17_fmt_add_stream_frame( dibits, pld );
    return n;
}
//
// Send a packet sub frame
// Payload must not exceed 25 bytes
// the FN field and EOF field and packet splitting
// will be done by the calling function.
//

int m17_fmt_add_packet(uint8_t *dibits, int len, bool eof, uint8_t nf, uint8_t *payload ){
	uint8_t tmp[80];
	/* 206 bits plus 4 flush encode to 420 bits, which also overran 388. */
	uint8_t txb[2][512];

	if( len <= 25){
		memset(tmp,0,25);
	    memcpy(tmp,payload,len);
	    if(eof == true)
	    	tmp[25]= 0x80;
	    else
	    	tmp[25] = 0x00;
	    tmp[25] |= (nf<<2);
		/* 206 type 1 bits: 200 of chunk plus 6 of metadata (EOF in bit 7 of
		 * the metadata byte, counter in bits 6..2).  Encoding the whole 26th
		 * byte would make 208 bits, and the four flush bits would then fall
		 * outside the 420 that P3 keeps - leaving the decoder's trellis
		 * unterminated and corrupting the metadata byte at the end of every
		 * frame, which is where the EOF flag lives. */
		uint8_t t1[206];
		int k = 0;
		for( int i = 0; i < 25; i++)
			for( int b = 7; b >= 0; b--) t1[k++] = (tmp[i] >> b) & 1;
		for( int b = 7; b >= 2; b--)    t1[k++] = (tmp[25] >> b) & 1;
		/* 206 bits in; the encoder appends the four flush bits and gives 420. */
		m17_conv_encode_1( t1, txb[0], 206);
		m17_punc_p3( txb[0], txb[1], 420);
		m17_interleave( txb[1], txb[0], 368);
		m17_de_correlate_1( txb[0], txb[1], 368);
		pack_16_to_2(SYNC_PACKET, dibits );
		pack_1_to_2(txb[1], &dibits[8],368);
		return FRAME_SYM_LENGTH;
	}
	return 0;
}
//
// Generate a BERT frame, the sequence should be reset at the start.
//
int m17_fmt_add_bert_frame(uint8_t *dibits ){
	uint8_t txb[2][402];
	m17_prbs9_tx_load(txb[0], 197);
	// Add the zero tail
	memset(&txb[0][197],0,sizeof(uint8_t)*4);
	m17_conv_encode_1(txb[0], txb[1],201);
	m17_punc_p2( txb[1], txb[0], 402);
	m17_interleave( txb[0], txb[1], 368);
	m17_de_correlate_1( txb[1], txb[0], 368);
	pack_16_to_2(SYNC_BERT, dibits );
	pack_1_to_2(txb[0], &dibits[8],368);
	return FRAME_SYM_LENGTH;
}
//
// End of transmission frame. 0x555D
//
int m17_fmt_add_eot(uint8_t *dibits){
	int idx = 0;
	for( int i = 0; i < 24; i++){
		dibits[idx++] = 0x01;
		dibits[idx++] = 0x01;
		dibits[idx++] = 0x01;
		dibits[idx++] = 0x01;
		dibits[idx++] = 0x01;
		dibits[idx++] = 0x01;
		dibits[idx++] = 0x03;
		dibits[idx++] = 0x01;
	}
	return idx;
}
//
// Used for testing only
//
void scramble_payload(uint8_t *in){
	for( int i = 0; i < 16; i++){
		in[i] = rand()&0xFF;
	}
}
//
// Main Interface
//

void m17_send_preamble(void){
    m17_fmt_add_tx_preamble(m_tx_d);
	m17_mod_dibits(m_tx_d, FRAME_SYM_LENGTH );
}

void m17_send_link_setup_frame(uint48_t dest, uint48_t src, M17Type type, uint8_t *meta){
	m_lich_count = 0;
	m_fn         = 0;

	gui_save_dest_address(dest);
	gui_save_src_address(src);

	m17_fmt_add_link_setup_frame(m_tx_d, dest, src, type, meta);
	m17_mod_dibits(m_tx_d, 192);
}
void m17_send_link_setup_frame_to_net(uint48_t dest, uint48_t src, M17Type type, uint8_t *meta){
	// Format a net frame, no sending
	m_net_fn = 0;
	gui_save_dest_address(dest);
	gui_save_src_address(src);
	build_lich_to_net(dest, src, type, meta);
}
void m17_send_link_setup_frame_fm_net(uint8_t *net){
	m_lich_count = 0;
	m_fn  = pack_8_to_16(&net[34]);

	m17_fmt_add_link_setup_frame_fm_net(m_tx_d, net);
	m17_mod_dibits(m_tx_d, 192);
}

void m17_send_stream_frame_to_net( uint8_t *payload ){
	m_net_fn = (m_net_fn+1);
	pack_16_to_8(m_net_fn, &m_net[34]);
	memcpy(&m_net[36],payload,16);
	uint16_t crc = m17_crc_array_encode( m_net, 52 );
    m_net[52] = (crc>>8);
    m_net[53] = (crc&0xFF);
	udp_send(m_net, 54 );
}

void m17_send_stream_frame( uint8_t *payload ){

	m17_fmt_add_stream_frame( m_tx_d, payload );
	m17_mod_dibits(m_tx_d, FRAME_SYM_LENGTH );
}
void m17_send_stream_frame_fm_net( uint8_t *net ){

	m17_fmt_add_stream_frame_fm_net( m_tx_d, net );
	m17_mod_dibits(m_tx_d, FRAME_SYM_LENGTH );
}

void m17_send_bert_frame( void ){
	m17_fmt_add_bert_frame( m_tx_d );
	m17_mod_dibits(m_tx_d, FRAME_SYM_LENGTH );
}
void m17_send_packet_frames( uint8_t *packet,  int len){

	// Add a 16 bit CRC (input buffer must be longer by 2 that the packet
	uint16_t crc = m17_crc_array_encode( packet, len );
	packet[len]   = (crc>>8);
	packet[len+1] = crc&0xFF;
	len += 2;

	int frames   = len/25;// Number of whole frames
	int leftover = len%25;// Size of partial frame

	if( leftover == 0){
		// Fits exactly
		for( int i = 0; i < frames -1; i++){
			m17_fmt_add_packet(m_tx_d, 25, false, i, &packet[i*25] );
			m17_mod_dibits(m_tx_d, FRAME_SYM_LENGTH );
		}
		// Send last full frame in sequence
		m17_fmt_add_packet(m_tx_d, 25, true, 25, &packet[(frames-1)*25]);
		m17_mod_dibits(m_tx_d, FRAME_SYM_LENGTH );
	}else{
		// Final frame is a partial one
		for( int i = 0; i < frames; i++){
			m17_fmt_add_packet(m_tx_d, 25, false, i, &packet[i*25] );
			m17_mod_dibits(m_tx_d, FRAME_SYM_LENGTH );
		}
		// Send last partial frame in sequence
		m17_fmt_add_packet(m_tx_d, leftover, true, leftover, &packet[frames*25]);
		m17_mod_dibits(m_tx_d, FRAME_SYM_LENGTH );
	}
}

void m17_send_eot(void){
	m17_fmt_add_eot(m_tx_d);
	m17_mod_dibits(m_tx_d, FRAME_SYM_LENGTH );
}

// Blank carrier
void m17_send_carrier(void){
	m17_mod_carrier();
}
void m17_fmt_init(void){
}
