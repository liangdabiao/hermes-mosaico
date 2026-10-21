// SPDX-License-Identifier: Apache-2.0
// Host tests for the credential store (Hermes v3) and the pure Hermes link
// helpers: ws:// URL splitting, JSON member reads, JSON escaping and the
// self-contained SHA-256 / HMAC / Base64 pinned to vectors produced by the
// server's own plugin/protocol.py.
#include "companion_credentials.h"
#include "hermes_wire.h"
#include "hermes_crypto.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static unsigned char stored[2048],pending[2048];
static size_t stored_size,pending_size;
static bool fail_write,fail_commit;

esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h)
{assert(!strcmp(name,"companion_keys"));*h=1;return !stored_size&&mode==NVS_READONLY?ESP_ERR_NVS_NOT_FOUND:ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size)
{(void)h;assert(!strcmp(key,"tokens"));if(*size<stored_size)return ESP_ERR_NVS_INVALID_LENGTH;memcpy(out,stored,stored_size);*size=stored_size;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t size)
{(void)h;assert(!strcmp(key,"tokens")&&size<sizeof(pending));if(fail_write)return 8;memcpy(pending,in,size);pending_size=size;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h)
{(void)h;if(fail_commit)return 9;memcpy(stored,pending,pending_size);stored_size=pending_size;return ESP_OK;}
void nvs_close(nvs_handle_t h){(void)h;}

static void credentials_test(void)
{
    companion_credentials_config_t c={"ws://hermes.home.arpa:8765/gadget",NULL};
    assert(companion_credentials_init()==ESP_OK&&!companion_credentials_ready());
    assert(companion_credentials_valid(&c));assert(companion_credentials_save(&c)==ESP_OK);
    assert(!companion_credentials_ready()); /* No concurrent snapshot mutation. */
    assert(companion_credentials_init()==ESP_OK&&companion_credentials_ready());
    assert(!strcmp(companion_credentials_server_url(),c.server_url));
    assert(companion_credentials_access_token()==NULL||!companion_credentials_access_token()[0]);
    /* The gadget endpoint is a WebSocket: ws and wss both pass, http does not. */
    c.server_url="wss://hermes.example.com:8443/gadget";assert(companion_credentials_valid(&c));
    c.server_url="http://hermes.example.com:8765";assert(!companion_credentials_valid(&c));
    c.server_url="ftp://hermes.example.com";assert(!companion_credentials_valid(&c));
    c.server_url="ws://";assert(!companion_credentials_valid(&c));
    c.server_url="ws://hermes.example.com/proxy@evil";assert(!companion_credentials_valid(&c));
    c.server_url="ws://hermes.example.com#frag";assert(!companion_credentials_valid(&c));
    c.server_url="ws://herm es.example.com";assert(!companion_credentials_valid(&c));
    char huge[COMPANION_SERVER_URL_CAP+1];memset(huge,'a',sizeof(huge)-1);huge[sizeof(huge)-1]=0;
    memcpy(huge,"ws://",5);c.server_url=huge;
    assert(!companion_credentials_valid(&c));
    c.server_url="ws://hermes.home.arpa:8765/gadget";
    /* The token is optional: absent, empty and a valid secret all pass. */
    c.access_token="";assert(companion_credentials_valid(&c));
    c.access_token="setec astronomy 42";assert(!companion_credentials_valid(&c)); /* spaces */
    c.access_token="setec-astronomy-42";assert(companion_credentials_valid(&c));
    c.access_token="bad\nnewline";assert(!companion_credentials_valid(&c));
    c.access_token=NULL;assert(companion_credentials_valid(&c));
    c.server_url="";assert(companion_credentials_save(&c)==ESP_ERR_INVALID_ARG);
    c.server_url="ws://hermes.home.arpa:8765/gadget";
    fail_write=true;assert(companion_credentials_save(&c)!=ESP_OK);fail_write=false;
    fail_commit=true;assert(companion_credentials_save(&c)!=ESP_OK);fail_commit=false;
    assert(companion_credentials_init()==ESP_OK);
    assert(!strcmp(companion_credentials_server_url(),"ws://hermes.home.arpa:8765/gadget"));
    c.access_token="second-secret";assert(companion_credentials_save(&c)==ESP_OK);
    assert(companion_credentials_init()==ESP_OK);
    assert(!strcmp(companion_credentials_access_token(),"second-secret"));
    /* Leftover records from older firmware are wiped instead of half-trusted. */
    assert(companion_credentials_save(&c)==ESP_OK&&companion_credentials_init()==ESP_OK);
    assert(companion_credentials_ready());
    stored[0]=2;assert(companion_credentials_init()==ESP_OK&&!companion_credentials_ready()); /* v2 Octop record */
    assert(companion_credentials_save(&c)==ESP_OK&&companion_credentials_init()==ESP_OK);
    assert(companion_credentials_ready());
    stored[4]='x'; /* Version fits, the URL scheme inside does not. */
    assert(companion_credentials_init()==ESP_OK&&!companion_credentials_ready());
    assert(companion_credentials_save(&c)==ESP_OK&&companion_credentials_init()==ESP_OK);
    assert(companion_credentials_ready());
    memset(stored,'x',24);stored_size=24; /* Truncated record of another shape. */
    assert(companion_credentials_init()==ESP_OK&&!companion_credentials_ready());
    memset(stored,'x',sizeof(stored));stored_size=sizeof(stored);
    assert(companion_credentials_init()==ESP_OK&&!companion_credentials_ready());
}

static void url_test(void)
{
    hermes_url_t u;
    assert(hermes_url_split("ws://h:8765/gadget",&u));
    assert(!strcmp(u.host,"h")&&u.port==8765&&!u.tls&&!strcmp(u.path,"/gadget"));
    assert(hermes_url_split("ws://hermes.home.arpa:9000/sub/gadget",&u));
    assert(!strcmp(u.host,"hermes.home.arpa")&&u.port==9000&&!strcmp(u.path,"/sub/gadget"));
    /* No path -> the plugin default; no port -> filled in by the transport. */
    assert(hermes_url_split("ws://hermes.home.arpa",&u));
    assert(u.port==0&&!strcmp(u.path,"/gadget"));
    assert(hermes_url_split("wss://hermes.example.com/gadget",&u));
    assert(u.tls&&!u.port);
    assert(!hermes_url_split("http://h/gadget",&u));
    assert(!hermes_url_split("ws://",&u));
    assert(!hermes_url_split("ws://host:0/gadget",&u));
    assert(!hermes_url_split("ws://host:99999/gadget",&u));
    char long_host[200];memset(long_host,'a',sizeof(long_host)-1);long_host[sizeof(long_host)-1]=0;
    char url[240];snprintf(url,sizeof(url),"ws://%s/gadget",long_host);
    assert(!hermes_url_split(url,&u));
}

static void json_test(void)
{
    /* Frame shapes lifted from tools/hermes_contract.json (live hub capture). */
    const char *welcome =
        "{\"type\": \"welcome\", \"session\": \"b40dc552bf84\", \"paired\": false, "
        "\"heartbeat_s\": 20, \"server\": \"hermes\", \"proto\": 1}";
    char text[512]; bool b; long n;
    assert(hermes_json_string(welcome,"type",text,sizeof(text))&&!strcmp(text,"welcome"));
    assert(hermes_json_bool(welcome,"paired",&b)&&!b);
    assert(hermes_json_long(welcome,"heartbeat_s",&n)&&n==20);
    const char *delta =
        "{\"type\":\"reply.delta\",\"text\":\"You said: 你好 Hermes\",\"turn\":\"abc:t1\"}";
    assert(hermes_json_string(delta,"text",text,sizeof(text)));
    assert(!strcmp(text,"You said: 你好 Hermes"));
    /* The same key inside a string value must not match, and nested objects
     * are not searched. */
    const char *tricky =
        "{\"type\":\"reply\",\"text\":\"the \\\"type\\\" here is not a key\","
        "\"extra\":{\"type\":\"noise\"}}";
    assert(hermes_json_string(tricky,"type",text,sizeof(text))&&!strcmp(text,"reply"));
    assert(hermes_json_string(tricky,"text",text,sizeof(text)));
    assert(!strcmp(text,"the \"type\" here is not a key"));
    const char *esc =
        "{\"type\":\"transcript\",\"text\":\"line\\nbreak \\u4f60\\u597d \\ttab\"}";
    assert(hermes_json_string(esc,"text",text,sizeof(text)));
    assert(!strcmp(text,"line\nbreak 你好 \ttab"));
    const char *pairing =
        "{\"type\":\"pairing\",\"code\":\"QUHDM37A\",\"command\":\"approve QUHDM37A\"}";
    assert(hermes_json_string(pairing,"code",text,sizeof(text))&&!strcmp(text,"QUHDM37A"));
    /* Escaping produces a valid JSON string body. */
    char out[128];
    assert(hermes_json_escape("say \"hi\" \\ done\n",out,sizeof(out)));
    assert(!strcmp(out,"say \\\"hi\\\" \\\\ done\\n"));
    assert(!hermes_json_escape("abc",out,3));   /* no room for the terminator */
}

static void crypto_test(void)
{
    /* Vectors generated by plugin/protocol.py + hashlib on the SDK side. */
    uint8_t key[32],digest[32];
    for (unsigned i=0;i<32;++i) key[i]=(uint8_t)((i*7+3)&0xFF);
    hermes_sha256((const uint8_t*)"abc",3,digest);
    char hex[65];hermes_hex(digest,32,hex);
    assert(!strcmp(hex,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    hermes_sha256((const uint8_t*)"",0,digest);
    hermes_hex(digest,32,hex);
    assert(!strcmp(hex,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    const char *msg="Hermes gadget v1 test vector. ";
    char longmsg[17*30+1];
    for (unsigned i=0;i<17;++i) memcpy(longmsg+i*30,msg,30);
    longmsg[sizeof(longmsg)-1]=0;
    hermes_sha256((const uint8_t*)longmsg,strlen(longmsg),digest);
    hermes_hex(digest,32,hex);
    assert(!strcmp(hex,"9402dcefe9f2b4d2178204951f73b0f9f0121f9370f98b8ff0ee3a8e1d7d4778"));
    char id[20];
    hermes_device_id(key,id);
    assert(!strcmp(id,"hg-ab5f8b5cb9435354"));
    /* auth_mac vectors: HMAC(key, "hermes-gadget/v1|"+device_id+"|"+nonce) */
    struct {const char *nonce,*mac;} macs[]={
        {"K1Qdb6/o18HrBshukv5miQ==","P07v/TeegFEA5O/KQxlAKcnps8Qf9+uYysUUHOxeCZs="},
        {"AAAAAAAAAAAAAAAAAAAAAA==","gScTch5BUpwFOWmU60s4Y0qDLY8Nba2TsXNiX1Z17YU="},
        {"short","pkh96tzA5KkRZ+IuzQNUQQeCcAeYQjwyWsqEuQg54oY="},
    };
    char body[160],b64[96];
    for (unsigned i=0;i<sizeof(macs)/sizeof(macs[0]);++i) {
        snprintf(body,sizeof(body),"hermes-gadget/v1|%s|%s",id,macs[i].nonce);
        hermes_hmac_sha256(key,sizeof(key),(const uint8_t*)body,strlen(body),digest);
        assert(hermes_base64_encode(digest,32,b64,sizeof(b64)));
        assert(!strcmp(b64,macs[i].mac));
    }
    /* Base64 round trip incl. all byte values 0..60 and padding shapes. */
    uint8_t raw[61],back[64];
    for (unsigned i=0;i<61;++i) raw[i]=(uint8_t)i;
    assert(hermes_base64_encode(raw,61,b64,sizeof(b64)));
    assert(!strcmp(b64,
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PA=="));
    assert(hermes_base64_decode(b64,back,sizeof(back))==61);
    assert(!memcmp(raw,back,61));
    assert(hermes_base64_decode("AAEC",back,sizeof(back))==3);
    assert(hermes_base64_decode("!!",back,sizeof(back))==(size_t)-1);
}

int main(void){
    credentials_test();url_test();json_test();crypto_test();
    puts("PASS: hermes credentials v3, ws url rules, json frames, crypto vectors");
    return 0;}
