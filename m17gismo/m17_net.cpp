#include <memory.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>

#include "m17defines.h"

#define UDP_PORT 17000

static uint8_t m_tx_buf[54];
static bool m_ref_active;
static bool m_udp_running;
static pthread_t m_tid[1];
static uint48_t m_from;
static char m_name[15];
static char m_ip_add[30];
static unsigned int m_port;
static char m_mod;
static uint16_t m_last_stream_id;
// When a voice frame last arrived *from the reflector*.  In client mode that is
// the only thing coming in - there is no radio - so it is what decides whether
// the station is shown as hearing somebody.
static uint32_t m_net_rx_ms;

static uint32_t net_now_ms(void){
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

int udp_send(uint8_t *b, int len );

void net_add_magic(void){
	m_tx_buf[0] = 0x4D;
	m_tx_buf[1] = 0x31;
	m_tx_buf[2] = 0x37;
	m_tx_buf[3] = 0x20;
}
void net_add_stream_id(uint16_t id){
	m_tx_buf[4] = (id>>8);
	m_tx_buf[5] = (id&0xFF);
}
void net_add_lich(uint8_t *lich){
	memcpy(&m_tx_buf[6],lich,28);
}
void net_add_fn(uint16_t fn){
	m_tx_buf[34] = (fn>>8);
	m_tx_buf[35] = (fn&0xFF);
}
void net_add_payload(uint8_t *pld){
	memcpy(&m_tx_buf[36],pld,16);
}
void net_add_crc(void){
	uint16_t crc = m17_crc_array_encode( m_tx_buf, 52 );
	m_tx_buf[52] = (crc>>8);
	m_tx_buf[53] = (crc&0xFF);
}
//
// Packets sent to network
//
bool m17_net_new_rx_data(uint16_t frame_id, uint8_t *lich, uint16_t fn, uint8_t *pld){

	// Overwrite the LICH dest callsign i,e with M17-M17
	char ref[20];
	uint8_t lh[54];
	memcpy( lh, lich, 54);
	sprintf(ref,"%s %c",m_name, m_mod);
	uint48_t call = m17_encode_call(ref);
	pack_48_to_8(call, &lh[0]);

	if(m_ref_active == true){
		net_add_magic();
	    net_add_stream_id(frame_id);
	    net_add_lich(lh);
	    net_add_fn(fn);
	    net_add_payload(pld);
	    net_add_crc();
	    // Send
	    udp_send(m_tx_buf, 54);
	}
	return m_ref_active;
}
//
// Packets used to talk to reflector
//
void net_pack_call(uint8_t *b, uint48_t a){
	b[0] = ((a>>40)&0xFF);
	b[1] = ((a>>32)&0xFF);
	b[2] = ((a>>24)&0xFF);
	b[3] = ((a>>16)&0xFF);
	b[4] = ((a>>8)&0xFF);
	b[5] = (a&0xFF);
}
int net_fmt_conn(uint8_t *b, uint48_t from, char mod){

	b[0] = 'C';
	b[1] = 'O';
	b[2] = 'N';
	b[3] = 'N';

	net_pack_call(&b[4], from);

	b[10] = mod;

	return 11;
}
int net_fmt_ackn(uint8_t *b){

	b[0] = 'A';
	b[1] = 'C';
	b[2] = 'K';
	b[3] = 'N';

	return 4;
}
int  net_fmt_nack(uint8_t *b){

	b[0] = 'N';
	b[1] = 'A';
	b[2] = 'C';
	b[3] = 'K';

	return 4;
}
int net_fmt_ping(uint8_t *b, uint48_t from){

	b[0] = 'P';
	b[1] = 'I';
	b[2] = 'N';
	b[3] = 'G';

	net_pack_call(&b[4], from);

	return 10;
}
int net_fmt_pong(uint8_t *b, uint48_t from){

	b[0] = 'P';
	b[1] = 'O';
	b[2] = 'N';
	b[3] = 'G';

	net_pack_call(&b[4], from);

	return 10;
}
int net_fmt_pong(uint8_t *b ){

	b[0] = 'P';
	b[1] = 'O';
	b[2] = 'N';
	b[3] = 'G';

	return 10;
}
int net_fmt_disc(uint8_t *b, uint48_t from){

	b[0] = 'D';
	b[1] = 'I';
	b[2] = 'S';
	b[3] = 'C';

	net_pack_call(&b[4], from);

	return 10;
}
int net_fmt_disc(uint8_t *b ){

	b[0] = 'D';
	b[1] = 'I';
	b[2] = 'S';
	b[3] = 'C';

	return 4;
}

static struct sockaddr_in m_me;
static struct sockaddr_in m_udp_other;
static int m_sock;

int udp_receive(uint8_t *b, int len){
	int recv_len;
	socklen_t slen;

	recv_len = recvfrom(m_sock, b, len, 0, (struct sockaddr *) &m_me, &slen);
	return recv_len;
}

int udp_send(uint8_t *b, int len ){
    return sendto(m_sock, b, len, 0,(struct sockaddr *) &m_udp_other, sizeof(m_udp_other));
}

int udp_tx_init( void )
{
    // Create a socket for transmitting UDP TS packets
    if ((m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
    {
        printf("Failed to create UDP TX socket\n");
        return -1;
    }
	int reuse = 1;
	setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int));
	// A reflector with nobody talking sends nothing at all, so a blocking recv
	// would never return and the "hearing ..." light would stay on for ever.
	// 200 ms lets the loop notice the silence.
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 200000;
	setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return 0;
}
int udp_rx_init( void )
{
	//create a UDP socket
    return 0;
}
void m17_parse_m17_data(uint8_t *b){
    if(m17_crc_array_encode( b, 54 ) == 0){
	    uint16_t stream,type,fn;
        uint48_t destin;
        uint48_t source;
        uint8_t *meta,*pld;
        // Skip magic
	    stream = pack_8_to_16(&b[4]);
	    destin = pack_8_to_48(&b[6]);
	    source = pack_8_to_48(&b[12]);
		gui_save_dest_address(destin);
		gui_save_src_address(source);
	    type = pack_8_to_16(&b[18]);
        meta = &b[20];// 14 bytes
	    fn = pack_8_to_16(&b[34]);
        pld  = &b[36];// Voice data
        // If the stream Id's are different then
        if(m_last_stream_id != stream) gui_update();
        // New PTT operation has occured
        m_last_stream_id = stream;
        if( m17_db_get_chan_type() == ASTODN ){
        	// Who is talking: the link setup fields are in every reflector frame,
        	// exactly as they are on the air, so the interface can show the
        	// station being heard.  Without this client mode played the audio but
        	// showed "no signal" over the PUSH TO TALK button.
        	m17_db_set_rx_src( source );
        	m17_db_set_rx_dst( destin );
        	m_net_rx_ms = net_now_ms();
        	if( m17_db_in_frame() == false ) m17_aos();
        	// Play audio on speakers
    		m17_txrx_spkr_audio( &pld[0] );
    		m17_txrx_spkr_audio( &pld[8]);
    		buff_rel(b);
        }
        if( m17_db_get_chan_type() == DRTODN ){
        	// Send to the radio
        	buff_post(b);
        }
    }
//	m17_decode_call(word, call[0]);
//	pack_8_to_48(&b[12], word);
//    m17_decode_call(word, call[1]);

}
// M17 Reflector

// IPv4: 152.70.192.70
// IPv6: 2603:c020:4005:4917:76ff:1f4e:3dca:9df4

void m17_net_parse_msg(uint8_t *b, int len){
	const char *m = (const char *)b;
	if(len < 4 ) return;

    if(strncmp(m,"CONN",4) == 0){
        //printf("FM Ref: CONN\n");
    	buff_rel(b);
        return;
    }
    if(strncmp(m,"ACKN",4) == 0){
        //printf("FM Ref: ACKN\n");
        m_ref_active = true;
    	buff_rel(b);
    	gui_update();
        return;
    }
    if(strncmp(m,"NACK",4) == 0){
        //printf("FM Ref: NACK\n");
    	buff_rel(b);
    	return;
    }

    if(strncmp(m,"PING",4) == 0){
        //printf("FM Ref: PING\n\r");
        // Send the pong
        int n = net_fmt_pong(b,m_from);
        udp_send(b, n);
    	buff_rel(b);
        return;
    }

    if(strncmp(m,"PONG",4) == 0){
        //printf("FM Ref: PONG\n");
    	buff_rel(b);
        return;
    }

    if(strncmp(m,"DISC",4) == 0){
        //printf("FM Ref: DISC\n");
        if(len > 4){
        	// DISC request, respond with DISC
            int n = net_fmt_disc(b);
            udp_send(b, n);
        }
        m_ref_active = false;
    	gui_update();
    	buff_rel(b);
		return;
    }

    if(strncmp(m,"M17 ",4) == 0){
    	// Handle incoming voice stream
    	//printf("Len %d\n",len);
    	m17_parse_m17_data(b);
        return;
    }
    // Catch all
	buff_rel(b);
}

void *udp_thread( void *arg){
	uint8_t *bp;
	while(m_udp_running == true){
		if((bp=buff_alloc())!= NULL){
	        int m = udp_receive(bp,54);
	        if(m > 0){
	            m17_net_parse_msg(bp, m);
	        }else{
	            // Timeout (nobody is talking) or an error: the buffer has to go
	            // back, or the pool runs dry, and then the silence is noticed.
	            buff_rel(bp);
	            if(m17_db_get_chan_type() == ASTODN && m17_db_in_frame() && m_net_rx_ms &&
	               (net_now_ms() - m_net_rx_ms) > 1200){
	                m_net_rx_ms = 0;
	                m17_los();
	            }
	        }
		}
	}
	return arg;
}
// Reflector names are written both ways.  The published list uses full names
// ("M17-M17"), older copies of M17Hosts.txt used the bare designator ("M17"),
// and operators type either.  Everything is therefore matched on the designator
// with any leading "M17-" removed, and displayed as "M17-<designator>".
static const char *ref_designator(const char *name){
	return (strncasecmp(name, "M17-", 4) == 0 && name[4] != 0) ? name + 4 : name;
}
int net_find_reflector(const char *name, char *ipadd, unsigned int *port){
	FILE *fp;
	char nname[40];
	char filename[] = {"M17Hosts.txt"};
	int res = -1;
	if((fp=fopen(filename,"r"))!= NULL ){
	    while(fscanf(fp,"%s %s %u",nname,ipadd,port)!=EOF){
            if(strcasecmp(ref_designator(name), ref_designator(nname))==0){
            	// Found
            	fclose(fp);
            	return 0;
            }
		}
	    fclose(fp);
	}else{
		char text[60];
		sprintf(text,"Unable to open %s",filename);
		gui_cmd_resp(text);
	}
	return res;
}
void m17_net_connect_to_reflector(const char *name, uint48_t fm, const char mod){

	// Save info
	char call[15];
	m17_decode_call(fm, call);
	// Only a *gateway* appends the letter G to its callsign.  A client carrying
	// its own audio must present the plain callsign: with the suffix the reflector
	// sees the audio arriving from a station that never connected (the audio
	// packets carry the plain callsign in their LSF) and drops it - the link looks
	// up, the meters move, and nothing is heard at the far end.
	if(m17_db_get_chan_type() == DRTODN) call[8] = 'G';
	m_from = m17_encode_call(call);
    m_mod  = mod;

	if(net_find_reflector(name, m_ip_add, &m_port)>= 0){
		//
		// Name it as the operator does, without doubling a leading "M17-"
		snprintf(m_name, sizeof(m_name), "M17-%s", ref_designator(name));
	    // Construct the client sockaddr_in structure
	    memset(&m_udp_other, 0, sizeof(m_udp_other));     // Clear struct
	    m_udp_other.sin_family      = AF_INET;            // Internet/IP
	    m_udp_other.sin_addr.s_addr = inet_addr(m_ip_add);  // IP address of reflector
	    m_udp_other.sin_port        = htons(m_port);    // server port
	    //
	    // Send connect
	    //
	    uint8_t tx_buffer[512];
	    int n;
	    n = net_fmt_conn(tx_buffer, m_from, mod);
	    n = udp_send(tx_buffer, n );

	    char text[160];
	    snprintf(text, sizeof(text), "connecting to %s module %c at %s:%u",
	             m_name, mod, m_ip_add, m_port);
	    printf("m17web: %s\n", text);
	    fflush(stdout);
	    gui_cmd_resp(text);
	}else{
	    // Say so: silently doing nothing looked like the button being broken
	    char text[160];
	    snprintf(text, sizeof(text), "reflector '%s' is not in M17Hosts.txt", name);
	    printf("m17web: %s\n", text);
	    fflush(stdout);
	    gui_cmd_resp(text);
	}
}
void m17_net_disconnect_from_reflector(void){

	uint8_t tx_buffer[512];
	if(m_udp_running == true) {
	    // Send Disconnect
	    int n = net_fmt_disc(tx_buffer, m_from);
        udp_send( tx_buffer, n );
	}
}
bool m17_net_connected(void){
	return m_ref_active;
}
void m17_net_ref_address(char *name){
	sprintf(name,"%11s %c",m_name,m_mod);
}
int m17_net_open(void)
{
	int r = 0;

	// Called on demand now (see mmi.cpp "conn"), so make it safe to repeat:
	// opening twice would try to bind the port a second time.
	if (m_udp_running == true) return 0;

	r += udp_rx_init();
	r += udp_tx_init();
	m_ref_active = false;

	if(pthread_create(&(m_tid[0]), NULL, &udp_thread, NULL)!=0){
        printf("UDP thread cannot be started\n\r");
        return -1;
    }
	m_udp_running = true;
	return r;
}

void m17_net_close(void)
{
	if(m_udp_running == true) {
	    m_udp_running = false;
	    close(m_sock);
	}
}


//
// Module character of the reflector we are connected to ('A'..'Z')
//
char m17_net_ref_module(void){
	return m_mod;
}
