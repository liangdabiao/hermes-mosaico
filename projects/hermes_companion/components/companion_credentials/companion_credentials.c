// SPDX-License-Identifier: Apache-2.0
#include "companion_credentials.h"
#include "nvs.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { CREDENTIALS_VERSION=3 };
typedef struct {
    uint32_t version;
    char url[COMPANION_SERVER_URL_CAP], token[COMPANION_TOKEN_CAP];
} credentials_t;
static credentials_t credentials;

static bool text_valid(const char *text,size_t cap)
{
    if(!text||!text[0])return false;
    size_t n=strnlen(text,cap);
    if(!n||n>=cap)return false;
    for(size_t i=0;i<n;++i){
        unsigned char c=(unsigned char)text[i];
        if(c<33||c>126)return false;
    }
    return true;
}
/* The Hermes gadget endpoint is a WebSocket: ws:// on a home LAN, wss://
 * when the host configured tls_cert. '@' and '#' stay banned because they
 * change how a URL is parsed and can smuggle credentials into the host part.
 * A path is expected (the plugin default is /gadget) but any valid one is
 * accepted so reverse-proxy deployments keep working. */
static bool url_valid(const char *url,size_t cap)
{
    if(!url||!url[0])return false;
    size_t n=strnlen(url,cap);
    if(!n||n>=cap)return false;
    for(size_t i=0;i<n;++i){
        unsigned char c=(unsigned char)url[i];
        if(c<33||c>126)return false;
    }
    bool wss=!strncmp(url,"wss://",6);
    size_t prefix=wss?6:5;
    if(!wss&&strncmp(url,"ws://",5))return false;
    return url[prefix]&&!strchr(url,'@')&&!strchr(url,'#');
}
bool companion_credentials_valid(const companion_credentials_config_t *c)
{
    if(!c)return false;
    /* The token is optional: an empty string means the host runs without
     * GADGET_ACCESS_TOKEN. Both "" and NULL validate as "absent". */
    return url_valid(c->server_url,COMPANION_SERVER_URL_CAP)&&
        (!c->access_token||!c->access_token[0]||
         text_valid(c->access_token,COMPANION_TOKEN_CAP));
}
static companion_credentials_config_t snapshot_of(const credentials_t *v)
{
    companion_credentials_config_t c;
    c.server_url=v->url;c.access_token=v->token;
    return c;
}
bool companion_credentials_ready(void)
{
    if(credentials.version!=CREDENTIALS_VERSION)return false;
    companion_credentials_config_t c=snapshot_of(&credentials);
    return companion_credentials_valid(&c);
}
esp_err_t companion_credentials_init(void)
{
    memset(&credentials,0,sizeof(credentials));
    nvs_handle_t h;
    esp_err_t err=nvs_open("companion_keys",NVS_READONLY,&h);
    if(err==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;
    if(err!=ESP_OK)return err;
    size_t size=sizeof(credentials);
    err=nvs_get_blob(h,"tokens",&credentials,&size);
    nvs_close(h);
    if(err==ESP_ERR_NVS_NOT_FOUND||err==ESP_ERR_NVS_INVALID_LENGTH){
        memset(&credentials,0,sizeof(credentials));return ESP_OK;}
    if(err!=ESP_OK)return err;
    /* A v2 record (Octop account) or anything truncated or half-valid falls
     * through to the wipe below: the device asks for a fresh Hermes setup
     * instead of half-trusting leftover values. */
    companion_credentials_config_t found=snapshot_of(&credentials);
    if(size!=sizeof(credentials)||credentials.version!=CREDENTIALS_VERSION||
        !companion_credentials_valid(&found))
        memset(&credentials,0,sizeof(credentials));
    return ESP_OK;
}
static void store(char *dst,size_t cap,const char *src)
{
    size_t n=strnlen(src,cap-1);
    memcpy(dst,src,n);dst[n]='\0';
}
esp_err_t companion_credentials_save(const companion_credentials_config_t *c)
{
    if(!companion_credentials_valid(c))return ESP_ERR_INVALID_ARG;
    credentials_t *next=calloc(1,sizeof(*next));
    if(!next)return ESP_ERR_NO_MEM;
    next->version=CREDENTIALS_VERSION;
    store(next->url,sizeof(next->url),c->server_url);
    if(c->access_token)store(next->token,sizeof(next->token),c->access_token);
    nvs_handle_t h;
    esp_err_t err=nvs_open("companion_keys",NVS_READWRITE,&h);
    if(err==ESP_OK){
        err=nvs_set_blob(h,"tokens",next,sizeof(*next));
        if(err==ESP_OK)err=nvs_commit(h);
        nvs_close(h);
    }
    /* Live readers keep the old snapshot. New values are loaded after reboot. */
    memset(next,0,sizeof(*next));free(next);
    return err;
}
const char *companion_credentials_server_url(void)
{return companion_credentials_ready()?credentials.url:NULL;}
const char *companion_credentials_access_token(void)
{return companion_credentials_ready()?credentials.token:NULL;}
