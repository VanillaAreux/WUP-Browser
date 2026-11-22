/* WUP Browser: a touch-first, lightweight HTML browser for libwup. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdbool.h>
#include "wup_lvgl.h"
#include "wup_https.h"

WUP_APP("WUP Browser");

#define URL_CAP 512
#define BODY_CAP (128u * 1024u)
#define TEXT_CAP 24000
#define LINK_CAP 16
#define HISTORY_CAP 6

typedef struct { char url[URL_CAP]; char label[128]; } browser_link_t;
typedef struct { char *data; size_t len, cap; int too_big; } response_t;

static lv_obj_t *address_bar, *keyboard, *title_label, *body_label, *links_list, *status_label;
static char current_url[URL_CAP], page_text[TEXT_CAP];
static browser_link_t page_links[LINK_CAP];
static int link_count, busy, net_ready, exit_requested;
static lv_obj_t *form_input, *active_input;
static char form_action[URL_CAP], form_field[80], form_placeholder[100], form_submit_label[48];
static int form_available;
static char history[HISTORY_CAP][URL_CAP];
static int history_count, history_pos = -1;

static void navigate(const char *input, int remember);
static void address_focus(lv_event_t *e);
static void navigate_later(const char *input, int remember);
static void status(const char *s) { if (status_label) lv_label_set_text(status_label, s); }
static void set_page(const char *title, const char *body) {
    if (title_label) lv_label_set_text(title_label, title);
    if (body_label) lv_label_set_text(body_label, body);
    if (links_list) lv_obj_clean(links_list);
    link_count = 0;
}
static void copy_s(char *d, size_t n, const char *s) {
    if (!n) return;
    if (!s) s = "";
    strncpy(d, s, n - 1); d[n - 1] = 0;
}
static int prefix(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static void trim_s(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
    size_t i = 0; while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s+i, strlen(s+i)+1);
}
static int collect_body(void *arg, const uint8_t *data, size_t n, long long total) {
    response_t *r = (response_t *)arg; (void)total;
    if (r->len + n > BODY_CAP) { r->too_big = 1; return 1; }
    if (r->len + n + 1 > r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 4096;
        while (cap < r->len + n + 1) cap *= 2;
        if (cap > BODY_CAP + 1) cap = BODY_CAP + 1;
        char *p = (char *)realloc(r->data, cap);
        if (!p) { r->too_big = 1; return 1; }
        r->data = p; r->cap = cap;
    }
    memcpy(r->data + r->len, data, n); r->len += n; r->data[r->len] = 0;
    return 0;
}
static void append_text(char *out, size_t cap, size_t *len, char c) {
    if (*len + 2 >= cap || c == '\r') return;
    if (c == '\n' || c == '\t' || c == ' ') {
        if (!*len || out[*len-1] == ' ' || out[*len-1] == '\n') return;
        out[(*len)++] = c == '\n' ? '\n' : ' ';
    } else out[(*len)++] = c;
    out[*len] = 0;
}
static char decode_entity(const char *s, size_t n) {
    if (n == 3 && !strncmp(s,"amp",3)) return '&';
    if (n == 2 && !strncmp(s,"lt",2)) return '<';
    if (n == 2 && !strncmp(s,"gt",2)) return '>';
    if (n == 4 && !strncmp(s,"quot",4)) return '"';
    if (n == 4 && !strncmp(s,"apos",4)) return '\'';
    if (n == 4 && !strncmp(s,"nbsp",4)) return ' ';
    return 0;
}
static void add_link(const char *href, const char *label) {
    if (link_count >= LINK_CAP || !href || !*href) return;
    static char u[URL_CAP]; copy_s(u, sizeof(u), href); trim_s(u);
    if (!*u || u[0] == '#' || prefix(u,"javascript:") || prefix(u,"mailto:") || prefix(u,"data:")) return;
    copy_s(page_links[link_count].url, URL_CAP, u);
    copy_s(page_links[link_count].label, sizeof(page_links[link_count].label), label && *label ? label : u);
    trim_s(page_links[link_count].label);
    link_count++;
}
static void attr_value(const char *tag, const char *name, char *out, size_t cap) {
    out[0] = 0; size_t nl = strlen(name);
    for (const char *p = tag; *p; p++) {
        if ((p == tag || !(isalnum((unsigned char)p[-1]) || p[-1] == '-')) &&
            !strncasecmp(p, name, nl)) {
            p += nl; while (*p && isspace((unsigned char)*p)) p++;
            if (*p++ != '=') continue;
            while (*p && isspace((unsigned char)*p)) p++;
            char q = 0; if (*p == '\'' || *p == '"') q = *p++;
            size_t i = 0;
            while (*p && ((q && *p != q) || (!q && !isspace((unsigned char)*p) && *p != '>')) && i+1 < cap) out[i++] = *p++;
            out[i] = 0; return;
        }
    }
}
static int block_tag(const char *tag) {
    static const char *b[] = {"p","div","br","hr","h1","h2","h3","h4","h5","h6","li","ul","ol","tr","table","section","article","header","footer","blockquote","pre","main"};
    char n[20]; size_t k=0; const char *p=tag;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p=='/') p++;
    while (*p && isalpha((unsigned char)*p) && k+1<sizeof(n)) n[k++]=(char)tolower((unsigned char)*p++);
    n[k]=0;
    for (unsigned i=0;i<sizeof(b)/sizeof(b[0]);i++) if (!strcmp(n,b[i])) return 1;
    return 0;
}
static void parse_html(const char *html) {
    size_t outn=0; page_text[0]=0; link_count=0;
    form_available=0; form_action[0]=form_field[0]=form_placeholder[0]=0; copy_s(form_submit_label,sizeof(form_submit_label),"Search");
    static char href[URL_CAP], link_text[128]; size_t link_n=0; href[0]=0; link_text[0]=0;
    int in_link=0, skip_script=0, skip_style=0;
    const char *p=html;
    while (*p && outn+4<TEXT_CAP) {
        if (*p=='<') {
            const char *end=strchr(p,'>'); if (!end) break;
            size_t n=(size_t)(end-p-1); static char tag[512]; if (n>=sizeof(tag)) n=sizeof(tag)-1;
            memcpy(tag,p+1,n); tag[n]=0;
            static char low[512]; copy_s(low,sizeof(low),tag);
            for (size_t i=0;low[i];i++) low[i]=(char)tolower((unsigned char)low[i]);
            if (prefix(low,"script") && low[6]!='/' && (low[6]==0 || isspace((unsigned char)low[6]))) skip_script=1;
            if (prefix(low,"/script")) skip_script=0;
            if (prefix(low,"style") && low[5]!='/' && (low[5]==0 || isspace((unsigned char)low[5]))) skip_style=1;
            if (prefix(low,"/style")) skip_style=0;
            if (!skip_script && !skip_style) {
                if (prefix(low,"form") && (low[4]==0 || isspace((unsigned char)low[4]))) { attr_value(tag,"action",form_action,sizeof(form_action)); form_available=1; }
                if (prefix(low,"input") && (low[5]==0 || isspace((unsigned char)low[5]) || low[5]=='/')) {
                    char typ[32], nm[80], val[100], ph[100];
                    attr_value(tag,"type",typ,sizeof(typ)); attr_value(tag,"name",nm,sizeof(nm)); attr_value(tag,"value",val,sizeof(val)); attr_value(tag,"placeholder",ph,sizeof(ph));
                    if (!strcasecmp(typ,"text") || !strcasecmp(typ,"search") || !*typ) { if (*nm && !*form_field) copy_s(form_field,sizeof(form_field),nm); if (*ph && !*form_placeholder) copy_s(form_placeholder,sizeof(form_placeholder),ph); }
                    else if (!strcasecmp(typ,"submit") && *val) copy_s(form_submit_label,sizeof(form_submit_label),val);
                }
                if (prefix(low,"a ") || !strcmp(low,"a")) {
                    attr_value(tag,"href",href,sizeof(href)); link_text[0]=0; link_n=0; in_link=1;
                } else if (prefix(low,"/a")) { if (in_link) add_link(href,link_text); in_link=0; }
                if (block_tag(tag)) {
                    if (prefix(low,"li") && (low[2]==0 || isspace((unsigned char)low[2]))) { append_text(page_text,TEXT_CAP,&outn,'\n'); append_text(page_text,TEXT_CAP,&outn,'•'); append_text(page_text,TEXT_CAP,&outn,' '); }
                    else if (prefix(low,"h") && low[1]>='1' && low[1]<='6') { append_text(page_text,TEXT_CAP,&outn,'\n'); append_text(page_text,TEXT_CAP,&outn,'▸'); append_text(page_text,TEXT_CAP,&outn,' '); }
                    else append_text(page_text,TEXT_CAP,&outn,'\n');
                }
                if (prefix(low,"/p") || prefix(low,"/h1") || prefix(low,"/h2") || prefix(low,"/h3") || prefix(low,"/li") || prefix(low,"/blockquote") || prefix(low,"/article")) { append_text(page_text,TEXT_CAP,&outn,'\n'); append_text(page_text,TEXT_CAP,&outn,'\n'); }
                if (prefix(low,"title") && (low[5]==0 || isspace((unsigned char)low[5]))) {
                    const char *te=strstr(end+1,"</title");
                    if (te && title_label) {
                        char t[180]; size_t tn=(size_t)(te-(end+1)); if(tn>=sizeof(t))tn=sizeof(t)-1;
                        memcpy(t,end+1,tn); t[tn]=0; trim_s(t); if(*t)lv_label_set_text(title_label,t);
                    }
                }
            }
            p=end+1; continue;
        }
        if (!skip_script && !skip_style) {
            const char *end=strchr(p,'<'); size_t n=end?(size_t)(end-p):strlen(p);
            for (size_t i=0;i<n;i++) {
                char c=p[i];
                if (c=='&') {
                    size_t j=i+1; while(j<n && j-i<=8 && p[j]!=';' && (isalnum((unsigned char)p[j])||p[j]=='#')) j++;
                    if(j<n && p[j]==';') { char d=decode_entity(p+i+1,j-i-1); if(d)c=d; i=(d?j:i); }
                }
                append_text(page_text,TEXT_CAP,&outn,c);
                if(in_link && link_n+2<sizeof(link_text)) {
                    if(isspace((unsigned char)c)) { if(link_n && link_text[link_n-1]!=' ')link_text[link_n++]=' '; }
                    else link_text[link_n++]=c;
                    link_text[link_n]=0;
                }
            }
            p+=n;
        } else p++;
    }
    trim_s(page_text);
    if (!*page_text) copy_s(page_text,sizeof(page_text),"This page has no readable text.");
}
static int absolute_url(const char *base, const char *href, char *out, size_t cap) {
    if (!href || !*href) return 0;
    if (prefix(href,"https://") || prefix(href,"http://")) { copy_s(out,cap,href); return 1; }
    if (prefix(href,"//")) { snprintf(out,cap,"https:%s",href); return 1; }
    const char *scheme=strstr(base,"://"); if(!scheme)return 0;
    const char *host=scheme+3, *path=strchr(host,'/');
    size_t origin=path?(size_t)(path-base):strlen(base);
    if(href[0]=='/') snprintf(out,cap,"%.*s%s",(int)origin,base,href);
    else if(!path) snprintf(out,cap,"%s/%s",base,href);
    else { const char *last=strrchr(path,'/'); size_t dir=last?(size_t)(last-base+1):origin+1; snprintf(out,cap,"%.*s%s",(int)dir,base,href); }
    return 1;
}
static void history_add(const char *url) {
    if(history_pos>=0 && !strcmp(history[history_pos],url))return;
    if(history_pos+1<history_count)history_count=history_pos+1;
    if(history_count==HISTORY_CAP) { for(int i=1;i<HISTORY_CAP;i++)copy_s(history[i-1],URL_CAP,history[i]); history_count--; history_pos--; }
    copy_s(history[history_count],URL_CAP,url); history_pos=history_count++;
}
static char pending_url[URL_CAP]; static int pending_remember, pending;
static void navigate_async_cb(void *arg) { (void)arg; pending = 0; navigate(pending_url, pending_remember); }
static void navigate_later(const char *input, int remember) {
    if (busy || pending || !input || !*input) return;
    copy_s(pending_url, sizeof(pending_url), input); pending_remember = remember; pending = 1;
    lv_async_call(navigate_async_cb, NULL);
}
static void idle_wait(void) { wup_sleep(2); }
static void link_clicked(lv_event_t *e) {
    if(busy)return;
    int i=(int)(intptr_t)lv_event_get_user_data(e); if(i<0||i>=link_count)return;
    static char target[URL_CAP]; if(absolute_url(current_url,page_links[i].url,target,sizeof(target)))navigate_later(target,1);
}
static void form_submit_clicked(lv_event_t *e) {
    (void)e;
    if (busy || !form_input || !form_available) return;
    const char *value=lv_textarea_get_text(form_input);
    if (!value || !*value) { status("Enter a search term first"); return; }
    char action[URL_CAP], encoded[URL_CAP], target[URL_CAP];
    if (form_action[0]) { if (!absolute_url(current_url,form_action,action,sizeof(action))) copy_s(action,sizeof(action),current_url); }
    else copy_s(action,sizeof(action),current_url);
    size_t n=0;
    for (const unsigned char *p=(const unsigned char *)value; *p && n+4<sizeof(encoded); p++) {
        if (isalnum(*p) || *p=='-' || *p=='_' || *p=='.' || *p=='~') encoded[n++]=(char)*p;
        else if (*p==' ') encoded[n++]='+';
        else { static const char hex[]="0123456789ABCDEF"; encoded[n++]='%'; encoded[n++]=hex[*p>>4]; encoded[n++]=hex[*p&15]; }
    }
    encoded[n]=0;
    snprintf(target,sizeof(target),"%s%c%s=%s",action,strchr(action,'?')?'&':'?',form_field[0]?form_field:"q",encoded);
    navigate_later(target,1);
}
static void show_links(void) {
    lv_obj_clean(links_list); form_input=NULL;
    if (form_available) {
        lv_obj_t *wrap=lv_obj_create(links_list); lv_obj_set_width(wrap,LV_PCT(100)); lv_obj_set_flex_flow(wrap,LV_FLEX_FLOW_ROW); lv_obj_set_style_pad_all(wrap,4,0);
        form_input=lv_textarea_create(wrap); lv_obj_set_flex_grow(form_input,1); lv_obj_set_height(form_input,44); lv_textarea_set_one_line(form_input,true); lv_textarea_set_placeholder_text(form_input,form_placeholder[0]?form_placeholder:"Search the web"); lv_obj_add_event_cb(form_input,address_focus,LV_EVENT_FOCUSED,NULL);
        lv_obj_t *submit=lv_button_create(wrap); lv_obj_set_height(submit,44); lv_obj_add_event_cb(submit,form_submit_clicked,LV_EVENT_CLICKED,NULL); lv_label_set_text(lv_label_create(submit),form_submit_label);
    }
    for(int i=0;i<link_count;i++) {
        lv_obj_t *b=lv_button_create(links_list); lv_obj_set_width(b,LV_PCT(100));
        lv_obj_add_event_cb(b,link_clicked,LV_EVENT_CLICKED,(void *)(intptr_t)i);
        lv_obj_t *l=lv_label_create(b); lv_label_set_text_fmt(l,LV_SYMBOL_RIGHT "  %s",page_links[i].label);
        lv_label_set_long_mode(l,LV_LABEL_LONG_DOT); lv_obj_set_width(l,LV_PCT(100));
    }
}
static void navigate(const char *input, int remember) {
    if(busy || !input || !*input)return;
    static char url[URL_CAP]; copy_s(url,sizeof(url),input); trim_s(url); if(!*url)return;
    if(prefix(url,"http://")) { static char tmp[URL_CAP]; snprintf(tmp,sizeof(tmp),"https://%s",url+7); copy_s(url,sizeof(url),tmp); }
    else if(!prefix(url,"https://")) { static char tmp[URL_CAP]; snprintf(tmp,sizeof(tmp),"https://%s",url); copy_s(url,sizeof(url),tmp); }
    if(!net_ready) {
        status("Starting network...");
        wup_log("[WUP Browser] starting network\n");
        net_ready = (wup_net_init() == 0);
        wup_log(net_ready ? "[WUP Browser] network ready\n" : "[WUP Browser] network unavailable\n");
    }
    if(!net_ready) { set_page("Wi-Fi unavailable","Join Wi-Fi in Homebrew Menu settings, then tap Go to retry."); status("Not connected"); return; }
    if(!wup_time_known()) { set_page("Clock not set","Connect to Wi-Fi in the Homebrew Menu first. The menu sets the clock, which is needed to verify HTTPS certificates."); status("HTTPS needs the system clock"); return; }
    busy=1; copy_s(current_url,sizeof(current_url),url); lv_textarea_set_text(address_bar,url);
    if(keyboard)lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN);
    if(remember)history_add(url);
    set_page("Loading...","Connecting securely. Please wait..."); status("Loading HTTPS page..."); lv_refr_now(NULL);
    response_t r={0};
    wup_https_opts opts={0};
    opts.timeout_ms=20000; opts.idle=idle_wait;
    opts.headers="User-Agent: WUPBrowser/1.0\r\nAccept: text/html,text/plain,application/xhtml+xml,*/*;q=0.5\r\nAccept-Encoding: identity\r\n";
    int code=wup_https_get(url,&opts,collect_body,&r);
    if(r.too_big) { set_page("Page too large","This page exceeded the 96 KiB download limit. Try a simpler or mobile version of the site."); status("Download limit reached"); }
    else if(code==200 && r.data) {
        lv_label_set_text(title_label,url); parse_html(r.data);
        lv_label_set_text(body_label,page_text); show_links(); status("Secure page loaded");
    } else {
        char msg[260];
        if(code==WUP_HTTPS_ENOTIME)copy_s(msg,sizeof(msg),"The system clock is not set. Join Wi-Fi from the Homebrew Menu and retry.");
        else if(code==WUP_HTTPS_EDNS)copy_s(msg,sizeof(msg),"The site name could not be resolved. Check the connection and address.");
        else if(code==WUP_HTTPS_ECONNECT)copy_s(msg,sizeof(msg),"Could not connect. The site may be offline or unavailable from this network.");
        else if(code==WUP_HTTPS_ETLS)snprintf(msg,sizeof(msg),"Secure connection or certificate verification failed (TLS %d).",wup_https_tls_error());
        else if(code==WUP_HTTPS_ETIMEOUT)copy_s(msg,sizeof(msg),"The website took too long to respond.");
        else if(code==WUP_HTTPS_EREDIRECT)copy_s(msg,sizeof(msg),"The site redirected too many times.");
        else if(code>=100)snprintf(msg,sizeof(msg),"The website returned HTTP status %d.",code);
        else snprintf(msg,sizeof(msg),"Could not load this page (browser error %d).",code);
        set_page("Couldn't load page",msg); status("Page load failed");
    }
    free(r.data); busy=0;
}
static void go_clicked(lv_event_t *e) { (void)e; navigate_later(lv_textarea_get_text(address_bar),1); }
static void reload_clicked(lv_event_t *e) { (void)e; if(*current_url)navigate_later(current_url,0); }
static void back_clicked(lv_event_t *e) { (void)e; if(!busy && history_pos>0) { history_pos--; navigate_later(history[history_pos],0); } }
static void home_clicked(lv_event_t *e) { (void)e; navigate_later("https://example.com",1); }
static void address_focus(lv_event_t *e) { active_input=lv_event_get_target(e); if(keyboard) { lv_keyboard_set_textarea(keyboard,active_input); lv_obj_clear_flag(keyboard,LV_OBJ_FLAG_HIDDEN); } }
static void exit_clicked(lv_event_t *e) { (void)e; exit_requested=1; }
static void keyboard_event(lv_event_t *e) {
    lv_event_code_t c=lv_event_get_code(e);
    if(c==LV_EVENT_READY) { lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN); if(active_input==form_input && form_available) form_submit_clicked(NULL); else navigate_later(lv_textarea_get_text(address_bar),1); }
    else if(c==LV_EVENT_CANCEL)lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN);
}

int main(void) {
    wup_lvgl_init();
    lv_obj_t *scr=lv_screen_active();
    lv_obj_set_flex_flow(scr,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr,8,0); lv_obj_set_style_pad_row(scr,5,0);

    lv_obj_t *bar=lv_obj_create(scr); lv_obj_set_size(bar,LV_PCT(100),52);
    lv_obj_set_flex_flow(bar,LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(bar,3,0); lv_obj_set_style_pad_column(bar,5,0);
    lv_obj_set_style_bg_color(bar,lv_color_hex(0x1B263B),0); lv_obj_set_style_bg_opa(bar,LV_OPA_COVER,0); lv_obj_set_style_radius(bar,12,0);
    lv_obj_t *back=lv_button_create(bar); lv_obj_set_size(back,44,42); lv_obj_add_event_cb(back,back_clicked,LV_EVENT_CLICKED,NULL);
    lv_label_set_text(lv_label_create(back),LV_SYMBOL_LEFT);
    lv_obj_t *reload=lv_button_create(bar); lv_obj_set_size(reload,44,42); lv_obj_add_event_cb(reload,reload_clicked,LV_EVENT_CLICKED,NULL);
    lv_label_set_text(lv_label_create(reload),LV_SYMBOL_REFRESH);
    address_bar=lv_textarea_create(bar); lv_obj_set_flex_grow(address_bar,1); lv_obj_set_height(address_bar,42);
    lv_textarea_set_one_line(address_bar,true); lv_textarea_set_placeholder_text(address_bar,"Enter website address");
    lv_textarea_set_text(address_bar,"https://example.com"); lv_obj_add_event_cb(address_bar,address_focus,LV_EVENT_FOCUSED,NULL);
    lv_obj_t *go=lv_button_create(bar); lv_obj_set_size(go,52,42); lv_obj_add_event_cb(go,go_clicked,LV_EVENT_CLICKED,NULL);
    lv_label_set_text(lv_label_create(go),"Go");
    lv_obj_t *exitb=lv_button_create(bar); lv_obj_set_size(exitb,58,42); lv_obj_add_event_cb(exitb,exit_clicked,LV_EVENT_CLICKED,NULL); lv_label_set_text(lv_label_create(exitb),"Exit");
    status_label=lv_label_create(scr); lv_label_set_text(status_label,"Ready");

    lv_obj_t *content=lv_obj_create(scr); lv_obj_set_width(content,LV_PCT(100)); lv_obj_set_flex_grow(content,1);
    lv_obj_set_flex_flow(content,LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_all(content,14,0); lv_obj_set_style_pad_row(content,10,0);
    lv_obj_set_style_bg_color(content,lv_color_hex(0xF5F7FB),0); lv_obj_set_style_bg_opa(content,LV_OPA_COVER,0); lv_obj_set_style_border_color(content,lv_color_hex(0xD9E0EA),0); lv_obj_set_style_radius(content,12,0);
    lv_obj_set_scroll_dir(content,LV_DIR_VER);
    title_label=lv_label_create(content); lv_obj_set_width(title_label,LV_PCT(100)); lv_obj_set_style_text_color(title_label,lv_color_hex(0x24344D),0); lv_label_set_text(title_label,"WUP Browser");
    body_label=lv_label_create(content); lv_obj_set_width(body_label,LV_PCT(100)); lv_obj_set_style_text_color(body_label,lv_color_hex(0x263238),0); lv_label_set_long_mode(body_label,LV_LABEL_LONG_WRAP);
    lv_label_set_text(body_label,"A lightweight browser for the GamePad. Type a website address above and tap Go.");
    links_list=lv_obj_create(content); lv_obj_set_width(links_list,LV_PCT(100)); lv_obj_set_flex_flow(links_list,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(links_list,0,0); lv_obj_set_style_pad_row(links_list,4,0);

    keyboard=lv_keyboard_create(scr); lv_keyboard_set_textarea(keyboard,address_bar);
    lv_obj_set_size(keyboard,LV_PCT(100),190); lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard,keyboard_event,LV_EVENT_READY,NULL); lv_obj_add_event_cb(keyboard,keyboard_event,LV_EVENT_CANCEL,NULL);
    set_page("WUP Browser","Enter a website address above and tap Go. Connect to Wi-Fi in the Homebrew Menu first.");
    while(!exit_requested) { wup_lvgl_run(10); if (wup_buttons() & WUP_HOME) exit_requested=1; }
    return 0;
}
