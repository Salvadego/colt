/*
 * colt - fast column aligner
 * ============================================================
 *
 * SYNOPSIS
 *   colt [OPTIONS] [FILE...]
 *
 * OPTIONS
 *   Delimiter (mutually exclusive; -e wins over -d):
 *     -d <str>     Literal delimiter string          (default: "=")
 *     -e <regex>   POSIX ERE regex delimiter
 *     -P <name>    Named preset from config file
 *
 *   Occurrence:
 *     -n <n>       Which match: 1,2,... | -1 = last | * = all  (default: 1)
 *
 *   Margins / layout:
 *     -l <n>       Left  margin around delimiter     (default: 1)
 *     -r <n>       Right margin around delimiter     (default: 1)
 *     -s           Stick delimiter to left token
 *     -a <m>       Token     alignment: l r c        (default: l)
 *     -D <m>       Delimiter alignment: l r c        (default: r)
 *     -i <m>       Indentation: k(eep) s(hallow) d(eep) n(one)  (default: k)
 *     -t <n>       Tab stop width                    (default: 8)
 *
 *   Table mode (replaces delimiter options):
 *     -T           Split on whitespace runs (like column -t)
 *     -S <sep>     Output column separator           (default: " ")
 *
 *   Filtering:
 *     -g <pat>     Only align lines matching ERE pattern
 *     -v <pat>     Skip  lines matching ERE pattern
 *     -x           Remove unmatched lines (default: preserve)
 *
 *   I/O:
 *     -j           JSON array input  (array of strings)
 *     -J           JSON array output
 *     -0           NUL-delimited output
 *
 *   Presets:
 *     --list-presets          Print all loaded presets and exit
 *     --config <file>         Load extra config file
 *     --dump-config           Print default config skeleton and exit
 *
 *   Misc:
 *     -h / --help             This help
 *
 * CONFIG FILE
 *   Loaded from (first found):
 *     $COLT_CONFIG
 *     ~/.config/colt/presets.toml
 *     ~/.colt.toml
 *
 *   Format: TOML-like, one [preset-name] section per preset.
 *   See --dump-config for a full annotated example.
 *
 * EXAMPLES
 *   colt -d=                      align first =
 *   colt -d= -n'*'                align all =
 *   colt -d: -s -l0 -r1           YAML/dict  key: value
 *   colt -e '>>|=>|>'             regex delimiter
 *   colt -P arrow                 named preset
 *   colt -T                       whitespace table (column -t style)
 *   colt -d'|' -n'*'              Markdown table
 *   colt -d= -ar                  right-align token before =
 *   colt -d= -g'^[^#]'            skip comment lines
 *   colt --list-presets           show available presets
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_LINE        16384
#define MAX_DELIM       512
#define MAX_NAME        128
#define MAX_LINES       1048576
#define MAX_PRESETS     256

static void die(const char *fmt, ...) {
        va_list ap; va_start(ap, fmt);
        fputs("colt: ", stderr);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        va_end(ap);
        exit(1);
}

static void warn(const char *fmt, ...) {
        va_list ap; va_start(ap, fmt);
        fputs("colt: warning: ", stderr);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        va_end(ap);
}

static void *xmalloc(size_t n) {
        void *p = malloc(n); if (!p) die("out of memory"); return p;
}
static void *xrealloc(void *p, size_t n) {
        p = realloc(p, n); if (!p) die("out of memory"); return p;
}
static char *xstrdup(const char *s) {
        char *p = xmalloc(strlen(s)+1); return strcpy(p, s);
}
static char *xstrndup(const char *s, size_t n) {
        char *p = xmalloc(n+1); memcpy(p,s,n); p[n]='\0'; return p;
}
static char *rtrim_inplace(char *s) {
        int n = (int)strlen(s);
        while (n > 0 && (unsigned char)s[n-1] <= ' ') s[--n] = '\0';
        return s;
}
static char *rtrimdup(const char *s) {
        int n = (int)strlen(s);
        while (n > 0 && (unsigned char)s[n-1] <= ' ') n--;
        return xstrndup(s, n);
}
static const char *ltrim(const char *s) {
        while (*s == ' ' || *s == '\t') s++;
        return s;
}
static char *spaces(int n) {
        if (n <= 0) return xstrdup("");
        char *s = xmalloc(n+1); memset(s,' ',n); s[n]='\0'; return s;
}
static int dw(const char *s, int tabstop) {
        int w = 0;
        for (; *s; s++) w += (*s=='\t') ? tabstop-(w%tabstop) : 1;
        return w;
}
static int file_exists(const char *p) {
        struct stat st; return stat(p, &st) == 0;
}

/* Safe bounded string copy: always NUL-terminates dst[0..dstsize-1].
   Uses memcpy so GCC's -Wstringop-truncation never fires. */
static void scopy(char *dst, const char *src, size_t dstsize) {
        size_t n = strlen(src);
        if (n >= dstsize) n = dstsize - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
}

typedef struct { char key[MAX_NAME]; char val[MAX_LINE]; } TomlEntry;
typedef struct { char name[MAX_NAME]; TomlEntry *e; int n, cap; } TomlSection;
typedef struct { TomlSection *s; int n, cap; } TomlDoc;

static void toml_init(TomlDoc *d) {
        d->cap=16; d->s=xmalloc(d->cap*sizeof(TomlSection)); d->n=0;
}
static TomlSection *toml_new_sec(TomlDoc *d, const char *name) {
        if (d->n>=d->cap){d->cap*=2;d->s=xrealloc(d->s,d->cap*sizeof(TomlSection));}
        TomlSection *s=&d->s[d->n++]; memset(s,0,sizeof*s);
        scopy(s->name, name, MAX_NAME); s->cap=8; s->e=xmalloc(s->cap*sizeof(TomlEntry));
        return s;
}
static void toml_add(TomlSection *s, const char *k, const char *v) {
        if (s->n>=s->cap){s->cap*=2;s->e=xrealloc(s->e,s->cap*sizeof(TomlEntry));}
        scopy(s->e[s->n].key, k, MAX_NAME); scopy(s->e[s->n].val, v, MAX_LINE); s->n++;
}
static const char *toml_get(const TomlSection *s, const char *k) {
        for(int i=0;i<s->n;i++) if(strcmp(s->e[i].key,k)==0) return s->e[i].val;
        return NULL;
}
static void toml_free(TomlDoc *d) {
        for(int i=0;i<d->n;i++) { free(d->s[i].e); }
        free(d->s);
}

static void toml_unquote(const char *src, char *dst, int dlen) {
        int i=0; if(*src=='"') src++;
        while(*src&&*src!='"'&&i<dlen-1){
                if(*src=='\\'&&*(src+1)){src++;
                        switch(*src){case '"':dst[i++]='"';break;case '\\':dst[i++]='\\';break;
                                case 'n':dst[i++]='\n';break;case 't':dst[i++]='\t';break;
                                default:dst[i++]='\\';dst[i++]=*src;}}
                else dst[i++]=*src;
                src++;
        }
        dst[i]='\0';
}

static int toml_parse_file(const char *path, TomlDoc *d) {
        FILE *f=fopen(path,"r"); if(!f) return 0;
        char line[MAX_LINE]; TomlSection *cur=NULL;
        while(fgets(line,sizeof line,f)){
                rtrim_inplace(line);
                const char *p=ltrim(line);
                if(*p=='#'||*p=='\0') continue;
                if(*p=='['){
                        p++;
                        const char *end=strchr(p,']');
                        if(!end){warn("bad section in %s",path);continue;}
                        char name[MAX_NAME];
                        size_t nl=(size_t)(end-p); if(nl>=MAX_NAME)nl=MAX_NAME-1;
                        memcpy(name,p,nl); name[nl]='\0';
                        char *np=name; while(*np==' '||*np=='\t') np++;
                        char *ne=np+strlen(np)-1; while(ne>np&&(*ne==' '||*ne=='\t'))*ne--='\0';
                        cur=toml_new_sec(d,np); continue;
                }
                if(!cur) continue;
                const char *eq=strchr(p,'='); if(!eq) continue;
                char key[MAX_NAME];
                size_t kl=(size_t)(eq-p);
                while(kl>0&&(p[kl-1]==' '||p[kl-1]=='\t'))kl--;
                if(!kl||kl>=MAX_NAME) continue;
                memcpy(key,p,kl); key[kl]='\0';
                const char *vp=ltrim(eq+1);
                char val[MAX_LINE];
                if(*vp=='"'){
                        toml_unquote(vp,val,sizeof val);
                } else if(*vp=='['){
                        const char *ap=vp+1; int vi=0;
                        while(*ap&&*ap!=']'){
                                while(*ap==' '||*ap==','||*ap=='\t')ap++;
                                if(*ap=='"'){
                                        char elem[MAX_NAME]; toml_unquote(ap,elem,sizeof elem);
                                        if(vi>0&&vi<MAX_LINE-2)val[vi++]=',';
                                        int el=(int)strlen(elem);
                                        if(vi+el<MAX_LINE-1){memcpy(val+vi,elem,el);vi+=el;}
                                        ap++; while(*ap&&*ap!='"')ap++; if(*ap=='"')ap++;
                                } else if(*ap&&*ap!=']'){
                                        const char *st=ap;
                                        while(*ap&&*ap!=','&&*ap!=']'&&*ap!=' ')ap++;
                                        int el=(int)(ap-st);
                                        if(vi>0&&vi<MAX_LINE-2)val[vi++]=',';
                                        if(vi+el<MAX_LINE-1){memcpy(val+vi,st,el);vi+=el;}
                                } else break;
                        }
                        val[vi]='\0';
                } else {
                        scopy(val, vp, MAX_LINE); rtrim_inplace(val);
                        char *hash=strchr(val,'#'); if(hash){*hash='\0';rtrim_inplace(val);}
                }
                toml_add(cur,key,val);
        }
        fclose(f); return 1;
}

typedef enum { NTH_NUMBER, NTH_LAST, NTH_NEG, NTH_ALL, NTH_ALT } NthMode;
typedef enum { ALIGN_L, ALIGN_R, ALIGN_C }     AlignMode;
typedef enum { IDT_KEEP, IDT_SHALLOW, IDT_DEEP, IDT_NONE } IndentMode;
typedef enum { DELIM_LITERAL, DELIM_REGEX } DelimType;

typedef struct {
        char       name[MAX_NAME];
        char       description[MAX_LINE];
        DelimType  delim_type;
        char       delim[MAX_DELIM];
        regex_t    delim_re;
        int        delim_compiled;
        int        lm, rm, stick;
        int        nth;
        NthMode    nth_mode;
        AlignMode  token_align;
        AlignMode  delim_align;
        IndentMode indent;
        int        tabstop;
        char       ignore_groups[MAX_LINE]; /* stored for Vim plugin passthrough */
} Preset;

static void preset_defaults(Preset *p) {
        memset(p,0,sizeof*p);
        strcpy(p->delim,"="); p->delim_type=DELIM_LITERAL;
        p->lm=1; p->rm=1; p->nth=1; p->nth_mode=NTH_NUMBER;
        p->token_align=ALIGN_L; p->delim_align=ALIGN_R;
        p->indent=IDT_KEEP; p->tabstop=8;
}
static int preset_compile(Preset *p) {
        if(p->delim_type!=DELIM_REGEX||p->delim_compiled) return 1;
        char errbuf[256];
        int r=regcomp(&p->delim_re,p->delim,REG_EXTENDED);
        if(r){regerror(r,&p->delim_re,errbuf,sizeof errbuf);
                warn("bad regex '%s': %s",p->name,errbuf); return 0;}
        p->delim_compiled=1; return 1;
}

static AlignMode  parse_am(const char *s, AlignMode  d){
        if(!s) return d;
        if(s[0]=='r'||s[0]=='R') return ALIGN_R;
        if(s[0]=='c'||s[0]=='C') return ALIGN_C;
        return ALIGN_L;
}
static IndentMode parse_im(const char *s, IndentMode d){
        if(!s) return d;
        if(s[0]=='s'||s[0]=='S') return IDT_SHALLOW;
        if(s[0]=='d'||s[0]=='D') return IDT_DEEP;
        if(s[0]=='n'||s[0]=='N') return IDT_NONE;
        return IDT_KEEP;
}

static void preset_from_toml(const TomlSection *sec, Preset *p) {
        preset_defaults(p);
        scopy(p->name, sec->name, MAX_NAME);
        const char *v;
        if((v=toml_get(sec,"description"))) scopy(p->description, v, MAX_LINE);
        if((v=toml_get(sec,"pattern")))   { scopy(p->delim, v, MAX_DELIM); p->delim_type=DELIM_REGEX; }
        if((v=toml_get(sec,"delimiter"))) { scopy(p->delim, v, MAX_DELIM); p->delim_type=DELIM_LITERAL; }
        if((v=toml_get(sec,"left_margin")))    p->lm=atoi(v);
        if((v=toml_get(sec,"right_margin")))   p->rm=atoi(v);
        if((v=toml_get(sec,"stick_to_left")))  p->stick=(strcmp(v,"true")==0||strcmp(v,"1")==0);
        if((v=toml_get(sec,"token_align")))    p->token_align=parse_am(v,ALIGN_L);
        if((v=toml_get(sec,"delimiter_align")))p->delim_align=parse_am(v,ALIGN_R);
        if((v=toml_get(sec,"indentation")))    p->indent=parse_im(v,IDT_KEEP);
        if((v=toml_get(sec,"tabstop")))        p->tabstop=atoi(v);
        if((v=toml_get(sec,"ignore_groups")))  scopy(p->ignore_groups, v, MAX_LINE);
        if((v=toml_get(sec,"nth"))){
                if(strcmp(v,"**")==0||strcmp(v,"alt")==0) p->nth_mode=NTH_ALT;
                else if(strcmp(v,"*")==0||strcmp(v,"all")==0) p->nth_mode=NTH_ALL;
                else if(strcmp(v,"last")==0) p->nth_mode=NTH_LAST;
                else{
                        int nv=atoi(v);
                        if(nv<0){
                                if(nv==-1) p->nth_mode=NTH_LAST;
                                else{p->nth_mode=NTH_NEG;p->nth=nv;}
                        } else {p->nth=nv;p->nth_mode=NTH_NUMBER;}
                }
        }
}

static const struct {
        const char *name, *desc, *delim; DelimType dt;
        int lm,rm,stick; AlignMode ta,da; NthMode nm;
} BUILTINS[] = {
        /* basic */
        {"eq",        "First =",                    "=",               DELIM_LITERAL,1,1,0,ALIGN_L,ALIGN_R,NTH_NUMBER},
        {"eq-all",    "All =",                       "=",               DELIM_LITERAL,1,1,0,ALIGN_L,ALIGN_R,NTH_ALL},
        {"colon",     "Colon - dict/YAML",           ":",               DELIM_LITERAL,0,1,1,ALIGN_L,ALIGN_R,NTH_NUMBER},
        {"comma",     "Comma",                        ",",               DELIM_LITERAL,0,1,1,ALIGN_L,ALIGN_R,NTH_NUMBER},
        {"comma-all", "All commas",                   ",",               DELIM_LITERAL,0,1,1,ALIGN_L,ALIGN_R,NTH_ALL},
        {"pipe",      "Pipe / Markdown table",        "|",               DELIM_LITERAL,1,1,0,ALIGN_L,ALIGN_R,NTH_ALL},
        {"space",     "Space",                        " ",               DELIM_LITERAL,0,0,0,ALIGN_L,ALIGN_R,NTH_NUMBER},
        {"dot",       "Dot / method chain",           "\\.",             DELIM_REGEX,  0,0,0,ALIGN_L,ALIGN_R,NTH_NUMBER},
        {"amp",       "Ampersand / LaTeX &",          "\\\\@<!&|\\\\\\\\",DELIM_REGEX, 1,1,0,ALIGN_L,ALIGN_R,NTH_ALL},
        /* from the user's vim config */
        {"arrow",     "Arrow operators >> => >",      ">>|=>|>",         DELIM_REGEX,  1,1,0,ALIGN_L,ALIGN_R,NTH_NUMBER},
        {"comment",   "C/C++ comments // /* */",      "//+|/\\*|\\*/",   DELIM_REGEX,  1,1,0,ALIGN_L,ALIGN_L,NTH_NUMBER},
        {"bracket",   "Square brackets [ ]",          "\\[|\\]",         DELIM_REGEX,  0,0,0,ALIGN_L,ALIGN_R,NTH_NUMBER},
        {"paren",     "Parentheses ( )",               "[()]",            DELIM_REGEX,  0,0,0,ALIGN_L,ALIGN_R,NTH_NUMBER},
        {"semi",      "Semicolon",                    ";",               DELIM_LITERAL,0,0,0,ALIGN_L,ALIGN_R,NTH_NUMBER},
        {"backslash", "Backslash line-continuation",  "\\\\",            DELIM_REGEX,  0,0,0,ALIGN_L,ALIGN_R,NTH_NUMBER},
        {"decl",      "C decl: space before id+[;=]"," (?=\\S+\\s*[;=])",DELIM_REGEX, 0,0,0,ALIGN_L,ALIGN_R,NTH_NUMBER},
};
#define N_BUILTINS (int)(sizeof BUILTINS/sizeof BUILTINS[0])

typedef struct {
        Preset active;
        int    table_mode;
        char   sep[MAX_DELIM];
        char   grep_pat[MAX_LINE];
        char   vgrep_pat[MAX_LINE];
        int    remove_unmatched;
        int    json_in, json_out, nul_out;
        Preset presets[MAX_PRESETS];
        int    npresets;
} Config;

static void config_init(Config *c) {
        memset(c,0,sizeof*c);
        preset_defaults(&c->active);
        strcpy(c->sep," ");
}

static void load_builtins(Config *c) {
        for(int i=0;i<N_BUILTINS&&c->npresets<MAX_PRESETS;i++){
                Preset *p=&c->presets[c->npresets++];
                preset_defaults(p);
                scopy(p->name, BUILTINS[i].name, MAX_NAME);
                scopy(p->description, BUILTINS[i].desc, MAX_LINE);
                scopy(p->delim, BUILTINS[i].delim, MAX_DELIM);
                p->delim_type =BUILTINS[i].dt;
                p->lm         =BUILTINS[i].lm;  p->rm=BUILTINS[i].rm;
                p->stick      =BUILTINS[i].stick;
                p->token_align=BUILTINS[i].ta;  p->delim_align=BUILTINS[i].da;
                p->nth_mode   =BUILTINS[i].nm;
        }
}

static void load_config_file(const char *path, Config *c) {
        TomlDoc doc; toml_init(&doc);
        if(!toml_parse_file(path,&doc)){warn("cannot read: %s",path);toml_free(&doc);return;}
        for(int i=0;i<doc.n;i++){
                if(strcmp(doc.s[i].name,"colt")==0) continue; /* reserved */
                /* overwrite existing preset with same name */
                int found=-1;
                for(int j=0;j<c->npresets;j++)
                        if(strcmp(c->presets[j].name,doc.s[i].name)==0){found=j;break;}
                Preset *p=(found>=0)?&c->presets[found]:&c->presets[c->npresets++];
                if(c->npresets>MAX_PRESETS){c->npresets=MAX_PRESETS;break;}
                preset_from_toml(&doc.s[i],p);
        }
        toml_free(&doc);
}

static char *default_config_path(void) {
        static char buf[4096];
        const char *env=getenv("COLT_CONFIG");
        if(env&&file_exists(env)){snprintf(buf,sizeof buf,"%s",env);return buf;}
        const char *home=getenv("HOME"); if(!home)home=".";
        snprintf(buf,sizeof buf,"%s/.config/colt/presets.toml",home);
        if(file_exists(buf))return buf;
        snprintf(buf,sizeof buf,"%s/.colt.toml",home);
        if(file_exists(buf))return buf;
        return NULL;
}

static void dump_default_config(void) {
        puts(
                        "# colt preset configuration\n"
                        "# Save to: ~/.config/colt/presets.toml\n"
                        "# Override location: $COLT_CONFIG\n"
                        "#\n"
                        "# Each [section-name] defines a preset usable with:  colt -P <name>\n"
                        "#\n"
                        "# Keys:\n"
                        "#   description     = \"Human readable label\"\n"
                        "#   delimiter       = \"=\"        # literal string\n"
                        "#   pattern         = \"=>|>>\"    # POSIX ERE regex (overrides delimiter)\n"
                        "#   nth             = 1          # 1,2,... | -1=last | *=all\n"
                        "#   left_margin     = 1\n"
                        "#   right_margin    = 1\n"
                        "#   stick_to_left   = false      # attach delim to left token\n"
                        "#   token_align     = \"l\"        # l / r / c\n"
                        "#   delimiter_align = \"r\"        # l / r / c\n"
                        "#   indentation     = \"k\"        # k(eep) s(hallow) d(eep) n(one)\n"
                        "#   tabstop         = 8\n"
                        "#   ignore_groups   = [\"String\", \"Comment\"]  # passed to Vim plugin\n"
                        "#\n"
                        ""
                        "\n"
                        "[arrow]\n"
                        "description    = \"Arrow operators: >> => >\"\n"
                        "pattern        = \">>|=>|>\"\n"
                        "\n"
                        "[comment]\n"
                        "description    = \"C/C++ comments //  /* */\"\n"
                        "pattern        = \"//+|/\\\\*|\\\\*/\"\n"
                        "delimiter_align = \"l\"\n"
                        "ignore_groups  = [\"!Comment\"]\n"
                        "\n"
                        "[bracket]\n"
                        "description    = \"Square brackets [ ]\"\n"
                        "pattern        = \"\\\\[|\\\\]\"\n"
                        "left_margin    = 0\n"
                        "right_margin   = 0\n"
                        "\n"
                        "[paren]\n"
                        "description    = \"Parentheses ( )\"\n"
                        "pattern        = \"[()]\"\n"
                        "left_margin    = 0\n"
                        "right_margin   = 0\n"
                        "\n"
                        "[semi]\n"
                        "description    = \"Semicolon\"\n"
                        "delimiter      = \";\"\n"
                        "left_margin    = 0\n"
                        "right_margin   = 0\n"
                        "\n"
                        "[backslash]\n"
                        "description    = \"Backslash line-continuation\"\n"
                        "pattern        = \"\\\\\\\\\"\n"
                        "left_margin    = 0\n"
                        "right_margin   = 0\n"
                        "\n"
                        "[decl]\n"
                        "description    = \"C declaration spacing\"\n"
                        "pattern        = \" (?=\\\\S+\\\\s*[;=])\"\n"
                        "left_margin    = 0\n"
                        "right_margin   = 0\n"
                        "\n"
                        "[colon]\n"
                        "description    = \"Colon - YAML / dict\"\n"
                        "delimiter      = \":\"\n"
                        "left_margin    = 0\n"
                        "right_margin   = 1\n"
                        "stick_to_left  = true\n"
                        "\n"
                        ""
                        "\n"
                        "# [my-preset]\n"
                        "# description = \"my custom rule\"\n"
                        "# pattern     = \"your-regex-here\"\n"
                        "# left_margin = 1\n"
                        "# right_margin = 1\n"
                        );
}

static void list_presets(const Config *c) {
        printf("%-22s  %-9s  %-34s  %s\n","NAME","TYPE","DESCRIPTION","PATTERN/DELIMITER");
        printf("%-22s  %-9s  %-34s  %s\n", "----------------------", "---------", "----------------------------------", "------------------");
        for(int i=0;i<c->npresets;i++){
                const Preset *p=&c->presets[i];
                printf("%-22s  %-9s  %-34s  %s\n",
                                p->name,
                                p->delim_type==DELIM_REGEX?"regex":"literal",
                                p->description[0]?p->description:"-",
                                p->delim);
        }
}


static void json_str(const char *s){
        putchar('"');
        for(;*s;s++){
                if(*s=='"')fputs("\\\"",stdout);
                else if(*s=='\\')fputs("\\\\",stdout);
                else if(*s=='\n')fputs("\\n",stdout);
                else if(*s=='\r')fputs("\\r",stdout);
                else if(*s=='\t')fputs("\\t",stdout);
                else if((unsigned char)*s<0x20)printf("\\u%04x",(unsigned char)*s);
                else putchar(*s);
        }
        putchar('"');
}
static char **parse_json_array(const char *src, int *count){
        int cap=64,n=0; char **arr=xmalloc(cap*sizeof(char*));
        const char *p=src;
        while(*p&&*p!='[') p++;
        if(!*p){*count=0;return arr;}
        p++;
        while(*p){
                while(*p&&*p!='"'&&*p!=']')p++;
                if(*p==']'||!*p) break;
                p++;
                char buf[MAX_LINE];int bi=0;
                while(*p&&*p!='"'){
                        if(*p=='\\'&&*(p+1)){p++;
                                switch(*p){case '"':buf[bi++]='"';break;case '\\':buf[bi++]='\\';break;
                                        case 'n':buf[bi++]='\n';break;case 'r':buf[bi++]='\r';break;
                                        case 't':buf[bi++]='\t';break;default:buf[bi++]=*p;}}
                        else buf[bi++]=*p;
                        if(bi>=MAX_LINE-1) break;
                        p++;
                }
                buf[bi]='\0'; if(*p=='"')p++;
                if(n>=cap){cap*=2;arr=xrealloc(arr,cap*sizeof(char*));}
                arr[n++]=xstrdup(buf);
        }
        *count=n; return arr;
}

typedef struct {
        char **tokens;  /* count tokens */
        char **delims;  /* count-1 delims (actual matched text) */
        int    count;
} Split;

static void split_free(Split *r){
        for(int i=0;i<r->count;i++)free(r->tokens[i]);
        for(int i=0;i<r->count-1;i++)free(r->delims[i]);
        free(r->tokens); free(r->delims);
}

/* Find one match starting at offset off.
   Sets *ms=start_idx, *me=end_idx (exclusive). Returns 1 on hit. */
static int find1(const char *s, int off, const Preset *p, int *ms, int *me){
        if(p->delim_type==DELIM_LITERAL){
                int dl=(int)strlen(p->delim); if(!dl)return 0;
                const char *f=strstr(s+off,p->delim); if(!f)return 0;
                *ms=(int)(f-s); *me=*ms+dl; return 1;
        } else {
                regmatch_t m; m.rm_so=off; m.rm_eo=(int)strlen(s);
                if(regexec(&p->delim_re,s,1,&m,REG_STARTEND)!=0) return 0;
                *ms=m.rm_so; *me=m.rm_eo;
                if(*ms==*me)(*me)++; /* avoid zero-length loop */
                return 1;
        }
}

static Split do_split(const char *line, const Preset *p){
        Split r; int cap=8;
        r.tokens=xmalloc(cap*sizeof(char*));
        r.delims=xmalloc(cap*sizeof(char*));
        r.count=0;
        int len=(int)strlen(line);

        if(p->nth_mode==NTH_ALL||p->nth_mode==NTH_ALT){
                int off=0,ms,me;
                while(find1(line,off,p,&ms,&me)){
                        if(r.count>=cap-1){cap*=2;
                                r.tokens=xrealloc(r.tokens,cap*sizeof(char*));
                                r.delims=xrealloc(r.delims,cap*sizeof(char*));}
                        r.tokens[r.count]=xstrndup(line+off,ms-off);
                        r.delims[r.count]=xstrndup(line+ms,me-ms);
                        r.count++; off=me; if(off>=len)break;
                }
                if(r.count>=cap){cap*=2;
                        r.tokens=xrealloc(r.tokens,cap*sizeof(char*));
                        r.delims=xrealloc(r.delims,cap*sizeof(char*));}
                r.tokens[r.count++]=xstrdup(line+off);
                return r;
        }

        if(p->nth_mode==NTH_LAST){
                /* collect all, use last */
                typedef struct{int s,e;}Pos;
                int pcap=16,pn=0; Pos *pos=xmalloc(pcap*sizeof(Pos));
                int off=0,ms,me;
                while(find1(line,off,p,&ms,&me)){
                        if(pn>=pcap){pcap*=2;pos=xrealloc(pos,pcap*sizeof(Pos));}
                        pos[pn].s=ms;pos[pn].e=me;pn++;off=me;if(off>=len)break;
                }
                if(pn==0){free(pos);goto no_match;}
                r.tokens[0]=xstrndup(line,pos[pn-1].s);
                r.delims[0]=xstrndup(line+pos[pn-1].s,pos[pn-1].e-pos[pn-1].s);
                r.tokens[1]=xstrdup(line+pos[pn-1].e);
                r.count=2; free(pos); return r;
        }

        /* NTH_NEG: nth from last (p->nth is negative, e.g. -2 means 2nd from last) */
        if(p->nth_mode==NTH_NEG){
                typedef struct{int s,e;}Pos2;
                int pcap=16,pn=0; Pos2 *pos=xmalloc(pcap*sizeof(Pos2));
                int off=0,ms,me;
                while(find1(line,off,p,&ms,&me)){
                        if(pn>=pcap){pcap*=2;pos=xrealloc(pos,pcap*sizeof(Pos2));}
                        pos[pn].s=ms;pos[pn].e=me;pn++;off=me;if(off>=len)break;
                }
                /* p->nth is e.g. -2: index from end = pn + p->nth  (p->nth < 0) */
                int idx = pn + p->nth;  /* e.g. pn=5, nth=-2 -> idx=3 */
                if(pn==0||idx<0){free(pos);goto no_match;}
                r.tokens[0]=xstrndup(line,pos[idx].s);
                r.delims[0]=xstrndup(line+pos[idx].s,pos[idx].e-pos[idx].s);
                r.tokens[1]=xstrdup(line+pos[idx].e);
                r.count=2; free(pos); return r;
        }

        /* NTH_NUMBER */
        {int off=0,ms,me,cnt=0;
                while(find1(line,off,p,&ms,&me)){
                        if(++cnt==p->nth){
                                r.tokens[0]=xstrndup(line,ms);
                                r.delims[0]=xstrndup(line+ms,me-ms);
                                r.tokens[1]=xstrdup(line+me);
                                r.count=2; return r;
                        }
                        off=me; if(off>=len)break;
                }}

no_match:
        r.tokens[0]=xstrdup(line); r.count=1; return r;
}

static Split do_split_ws(const char *line){
        Split r; int cap=16;
        r.tokens=xmalloc(cap*sizeof(char*));
        r.delims=xmalloc(cap*sizeof(char*));
        r.count=0;
        const char *p=line;
        /* preserve leading whitespace in first token */
        const char *body=p; while(*body==' '||*body=='\t')body++;
        size_t plen=(size_t)(body-p); char prefix[MAX_LINE];
        memcpy(prefix,p,plen); prefix[plen]='\0'; p=body;
        while(*p){
                const char *st=p; while(*p&&*p!=' '&&*p!='\t')p++;
                char *tok;
                if(r.count==0&&plen){
                        tok=xmalloc(plen+(p-st)+1);
                        memcpy(tok,prefix,plen); memcpy(tok+plen,st,p-st);
                        tok[plen+(p-st)]='\0';
                } else tok=xstrndup(st,p-st);
                if(r.count>=cap-1){cap*=2;
                        r.tokens=xrealloc(r.tokens,cap*sizeof(char*));
                        r.delims=xrealloc(r.delims,cap*sizeof(char*));}
                if(r.count>0)r.delims[r.count-1]=xstrdup(" ");
                r.tokens[r.count++]=tok;
                while(*p==' '||*p=='\t')p++;
        }
        if(!r.count){r.tokens[r.count++]=xstrdup(line);}
        return r;
}

typedef struct { char *raw; Split split; int filtered; } Line;

static char *pad_to(const char *tok, int tw, AlignMode m, int ts){
        int w=dw(tok,ts), pad=tw-w; if(pad<0)pad=0;
        char *out;
        switch(m){
                case ALIGN_R:{
                                     const char *bd=ltrim(tok); int il=(int)(bd-tok);
                                     out=xmalloc(il+pad+strlen(bd)+1);
                                     memcpy(out,tok,il); memset(out+il,' ',pad); strcpy(out+il+pad,bd); break;}
                case ALIGN_C:{int lp=pad/2,rp=pad-lp;
                                     out=xmalloc(lp+strlen(tok)+rp+1);
                                     memset(out,' ',lp); memcpy(out+lp,tok,strlen(tok));
                                     memset(out+lp+strlen(tok),' ',rp); out[lp+strlen(tok)+rp]='\0'; break;}
                default:
                             out=xmalloc(strlen(tok)+pad+1);
                             strcpy(out,tok); memset(out+strlen(tok),' ',pad); out[strlen(tok)+pad]='\0';
        }
        return out;
}

static void emit(const char *s, int nul){ fputs(s,stdout); putchar(nul?'\0':'\n'); }

static void align_single(Line *lines, int n, const Config *cfg){
        const Preset *p=&cfg->active;
        int max_bw=0, min_ind=-1, max_ind=0;
        for(int i=0;i<n;i++){
                Line *l=&lines[i]; if(l->filtered||l->split.count<2)continue;
                char *bt=rtrimdup(l->split.tokens[0]); int bw=dw(bt,p->tabstop); free(bt);
                if(bw>max_bw)max_bw=bw;
                const char *q=l->split.tokens[0]; int ind=0;
                while(*q==' '||*q=='\t'){ind+=(*q=='\t')?p->tabstop-(ind%p->tabstop):1;q++;}
                if(min_ind<0||ind<min_ind) min_ind=ind;
                if(ind>max_ind) max_ind=ind;
        }
        if(min_ind<0)min_ind=0;
        int ti=-1;
        switch(p->indent){case IDT_SHALLOW:ti=min_ind;break;case IDT_DEEP:ti=max_ind;break;case IDT_NONE:ti=0;break;default:break;}
        char *lm=spaces(p->lm), *rm=spaces(p->rm);
        for(int i=0;i<n;i++){
                Line *l=&lines[i];
                if(l->filtered||l->split.count<2){if(!cfg->remove_unmatched)emit(l->raw,cfg->nul_out);continue;}
                char *braw=rtrimdup(l->split.tokens[0]), *before;
                if(ti>=0){
                        const char *body=ltrim(braw); char *ind=spaces(ti);
                        before=xmalloc(ti+strlen(body)+1); memcpy(before,ind,ti); strcpy(before+ti,body); free(ind);
                } else before=xstrdup(braw);
                free(braw);
                int pad=max_bw-dw(before,p->tabstop); if(pad<0)pad=0;
                const char *ad=l->split.delims[0];
                char *after=rtrimdup(ltrim(l->split.tokens[1]));
                if(p->stick) printf("%s%*s%s%s%s%s",before,pad,"",ad,lm,rm,after);
                else         printf("%s%*s%s%s%s%s",before,pad,"",lm,ad,rm,after);
                putchar(cfg->nul_out?'\0':'\n');
                free(before); free(after);
        }
        free(lm); free(rm);
}

static void align_star(Line *lines, int n, const Config *cfg){
        const Preset *p=&cfg->active;
        int mc=0;
        for(int i=0;i<n;i++){if(lines[i].filtered)continue;if(lines[i].split.count>mc)mc=lines[i].split.count;}
        int *cmax=xmalloc(mc*sizeof(int)); memset(cmax,0,mc*sizeof(int));
        for(int i=0;i<n;i++){
                if(lines[i].filtered)continue;
                Split *sr=&lines[i].split;
                for(int c=0;c<sr->count;c++){
                        const char *tok=(c==0)?sr->tokens[0]:ltrim(sr->tokens[c]);
                        char *t=rtrimdup(tok); int w=dw(t,p->tabstop); free(t);
                        if(w>cmax[c])cmax[c]=w;
                }
        }
        char *lm=spaces(p->lm), *rm=spaces(p->rm);
        for(int i=0;i<n;i++){
                Line *l=&lines[i];
                if(l->filtered){emit(l->raw,cfg->nul_out);continue;}
                Split *sr=&l->split;
                if(sr->count<2){emit(l->raw,cfg->nul_out);continue;}
                for(int c=0;c<sr->count;c++){
                        const char *rt=(c==0)?sr->tokens[c]:ltrim(sr->tokens[c]);
                        char *t=rtrimdup(rt);
                        if(c==sr->count-1){fputs(t,stdout);}
                        else{
                                char *pd=pad_to(t,cmax[c],p->token_align,p->tabstop);
                                const char *ad=sr->delims[c];
                                if(p->stick) printf("%s%s%s%s",pd,ad,lm,rm);
                                else         printf("%s%s%s%s",pd,lm,ad,rm);
                                free(pd);
                        }
                        free(t);
                }
                putchar(cfg->nul_out?'\0':'\n');
        }
        free(lm); free(rm); free(cmax);
}

/* Alternating L/R alignment across all delimiters (** mode).
   Column 0,2,4,... gets left-aligned token; column 1,3,5,... gets right-aligned. */
static void align_alt(Line *lines, int n, const Config *cfg){
        const Preset *p=&cfg->active;
        int mc=0;
        for(int i=0;i<n;i++){
                if(lines[i].filtered)continue;
                if(lines[i].split.count>mc)mc=lines[i].split.count;
        }
        int *cmax=xmalloc(mc*sizeof(int)); memset(cmax,0,mc*sizeof(int));
        for(int i=0;i<n;i++){
                if(lines[i].filtered)continue;
                Split *sr=&lines[i].split;
                for(int c=0;c<sr->count;c++){
                        const char *tok=(c==0)?sr->tokens[0]:ltrim(sr->tokens[c]);
                        char *t=rtrimdup(tok); int w=dw(t,p->tabstop); free(t);
                        if(w>cmax[c])cmax[c]=w;
                }
        }
        char *lm=spaces(p->lm), *rm=spaces(p->rm);
        for(int i=0;i<n;i++){
                Line *l=&lines[i];
                if(l->filtered){emit(l->raw,cfg->nul_out);continue;}
                Split *sr=&l->split;
                if(sr->count<2){emit(l->raw,cfg->nul_out);continue;}
                for(int c=0;c<sr->count;c++){
                        const char *rt=(c==0)?sr->tokens[c]:ltrim(sr->tokens[c]);
                        char *t=rtrimdup(rt);
                        int is_last=(c==sr->count-1);
                        if(is_last){
                                fputs(t,stdout);
                        } else {
                                /* Even columns (0,2,...) left-align; odd columns (1,3,...) right-align */
                                AlignMode am = (c%2==0) ? ALIGN_L : ALIGN_R;
                                char *pd=pad_to(t,cmax[c],am,p->tabstop);
                                const char *ad=sr->delims[c];
                                if(p->stick) printf("%s%s%s%s",pd,ad,lm,rm);
                                else         printf("%s%s%s%s",pd,lm,ad,rm);
                                free(pd);
                        }
                        free(t);
                }
                putchar(cfg->nul_out?'\0':'\n');
        }
        free(lm); free(rm); free(cmax);
}

static void align_table(Line *lines, int n, const Config *cfg){
        const Preset *p=&cfg->active;
        int mc=0;
        for(int i=0;i<n;i++){if(lines[i].filtered)continue;if(lines[i].split.count>mc)mc=lines[i].split.count;}
        int *cmax=xmalloc(mc*sizeof(int)); memset(cmax,0,mc*sizeof(int));
        for(int i=0;i<n;i++){
                if(lines[i].filtered)continue;
                Split *sr=&lines[i].split;
                for(int c=0;c<sr->count;c++){
                        char *t=rtrimdup(sr->tokens[c]); int w=dw(t,p->tabstop); free(t);
                        if(w>cmax[c])cmax[c]=w;
                }
        }
        for(int i=0;i<n;i++){
                Line *l=&lines[i];
                if(l->filtered){emit(l->raw,cfg->nul_out);continue;}
                Split *sr=&l->split;
                for(int c=0;c<sr->count;c++){
                        char *t=rtrimdup(sr->tokens[c]); int w=dw(t,p->tabstop);
                        fputs(t,stdout);
                        if(c<sr->count-1){for(int k=0;k<cmax[c]-w;k++)putchar(' ');fputs(cfg->sep,stdout);}
                        free(t);
                }
                putchar(cfg->nul_out?'\0':'\n');
        }
        free(cmax);
}

static char **read_lines_f(FILE *f, int *count){
        int cap=1024,n=0; char **arr=xmalloc(cap*sizeof(char*)); char buf[MAX_LINE];
        while(fgets(buf,sizeof buf,f)){
                if(n>=cap){cap*=2;arr=xrealloc(arr,cap*sizeof(char*));}
                arr[n++]=xstrdup(rtrim_inplace(buf));
        }
        *count=n; return arr;
}

static void usage(void){
        puts(
                        "colt - fast column aligner\n"
                        "\n"
                        "Usage: colt [OPTIONS] [FILE...]\n"
                        "\n"
                        "Delimiter:\n"
                        "  -d <str>       Literal string                    (default: =)\n"
                        "  -e <regex>     POSIX ERE regex\n"
                        "  -P <name>      Named preset  (--list-presets)\n"
                        "\n"
                        "Occurrence:\n"
                        "  -n <n>         1,2,... | -1=last | -2=2nd-last | *=all | **=alt  (default: 1)\n"
                        "\n"
                        "Layout:\n"
                        "  -l <n>         Left  margin                      (default: 1)\n"
                        "  -r <n>         Right margin                      (default: 1)\n"
                        "  -s             Stick delimiter to left token\n"
                        "  -a l|r|c       Token     alignment               (default: l)\n"
                        "  -D l|r|c       Delimiter alignment               (default: r)\n"
                        "  -i k|s|d|n     Indentation                       (default: k=keep)\n"
                        "  -t <n>         Tab stop width                    (default: 8)\n"
                        "\n"
                        "Table mode:\n"
                        "  -T             Split on whitespace (column -t style)\n"
                        "  -S <sep>       Output separator                  (default: ' ')\n"
                        "\n"
                        "Filter:\n"
                        "  -g <pat>       Only align matching lines\n"
                        "  -v <pat>       Skip matching lines\n"
                        "  -x             Drop unmatched lines\n"
                        "\n"
                        "I/O:\n"
                        "  -j / -J        JSON array input / output\n"
                        "  -0             NUL-delimited output\n"
                        "\n"
                        "Presets:\n"
                        "  --list-presets      All available presets\n"
                        "  --dump-config       Print config skeleton  (~/.config/colt/presets.toml)\n"
                        "  --config <file>     Load additional config\n"
                        "\n"
                        "Examples:\n"
                        "  colt -d=                   first =\n"
                        "  colt -d= -n'*'             all =\n"
                        "  colt -P arrow              >> => > (preset)\n"
                        "  colt -e '//+|/\\*|\\*/'      regex\n"
                        "  colt -d: -s -l0 -r1        YAML\n"
                        "  colt -T                    whitespace table\n"
                        "  colt --list-presets\n"
                        );
        exit(0);
}

static void parse_args(int argc, char **argv, Config *cfg, char ***files, int *nfiles){
        int cap=16; *files=xmalloc(cap*sizeof(char*)); *nfiles=0;
        for(int i=1;i<argc;i++){
                char *a=argv[i];
                if(strncmp(a,"--",2)==0){
                        if(strcmp(a,"--help")==0)usage();
                        if(strcmp(a,"--list-presets")==0){list_presets(cfg);exit(0);}
                        if(strcmp(a,"--dump-config")==0){dump_default_config();exit(0);}
                        if(strcmp(a,"--config")==0){
                                if(i+1>=argc)die("--config needs argument");
                                load_config_file(argv[++i],cfg); continue;
                        }
                        if(strcmp(a,"--")==0){
                                for(i++;i<argc;i++){
                                        if(*nfiles>=cap){cap*=2;*files=xrealloc(*files,cap*sizeof(char*));}
                                        (*files)[(*nfiles)++]=argv[i];} break;
                        }
                        die("unknown option: %s",a);
                }
                if(a[0]=='-'&&a[1]){
                        char flag=a[1];
                        switch(flag){
                                case 's':cfg->active.stick=1;continue;
                                case 'T':cfg->table_mode=1; continue;
                                case 'p':cfg->remove_unmatched=0;continue;
                                case 'x':cfg->remove_unmatched=1;continue;
                                case 'j':cfg->json_in=1;  continue;
                                case 'J':cfg->json_out=1; continue;
                                case '0':cfg->nul_out=1;  continue;
                                case 'h':usage(); break;
                                default: break;
                        }
                        char *val=(a[2]!='\0')?&a[2]:(i+1<argc?argv[++i]:NULL);
                        switch(flag){
                                case 'd':
                                        if(!val)die("-d needs argument");
                                        scopy(cfg->active.delim, val, MAX_DELIM);
                                        cfg->active.delim_type=DELIM_LITERAL; break;
                                case 'e':
                                        if(!val)die("-e needs argument");
                                        scopy(cfg->active.delim, val, MAX_DELIM);
                                        cfg->active.delim_type=DELIM_REGEX; break;
                                case 'P':{
                                                 if(!val)die("-P needs argument");
                                                 int found=0;
                                                 for(int j=0;j<cfg->npresets;j++){
                                                         if(strcmp(cfg->presets[j].name,val)==0){
                                                                 int ts=cfg->active.tabstop;
                                                                 cfg->active=cfg->presets[j];
                                                                 cfg->active.tabstop=ts;
                                                                 found=1; break;
                                                         }
                                                 }
                                                 if(!found)die("unknown preset: %s (try --list-presets)",val);
                                                 break;
                                         }
                                case 'n':
                                         if(!val)die("-n needs argument");
                                         if(strcmp(val,"**")==0||strcmp(val,"alt")==0)cfg->active.nth_mode=NTH_ALT;
                                         else if(strcmp(val,"*")==0||strcmp(val,"all")==0)cfg->active.nth_mode=NTH_ALL;
                                         else if(strcmp(val,"last")==0){cfg->active.nth_mode=NTH_LAST;}
                                         else{
                                                 int nv=atoi(val);
                                                 if(nv<0){
                                                         if(nv==-1){cfg->active.nth_mode=NTH_LAST;}
                                                         else{cfg->active.nth_mode=NTH_NEG;cfg->active.nth=nv;}
                                                 } else {
                                                         cfg->active.nth_mode=NTH_NUMBER;
                                                         cfg->active.nth=nv<1?1:nv;
                                                 }
                                         }
                                         break;
                                case 'l':if(!val)die("-l needs argument");cfg->active.lm=atoi(val);if(cfg->active.lm<0)cfg->active.lm=0;break;
                                case 'r':if(!val)die("-r needs argument");cfg->active.rm=atoi(val);if(cfg->active.rm<0)cfg->active.rm=0;break;
                                case 'a':if(!val)die("-a needs argument");cfg->active.token_align=parse_am(val,ALIGN_L);break;
                                case 'D':if(!val)die("-D needs argument");cfg->active.delim_align=parse_am(val,ALIGN_R);break;
                                case 'i':if(!val)die("-i needs argument");cfg->active.indent=parse_im(val,IDT_KEEP);break;
                                case 't':if(!val)die("-t needs argument");cfg->active.tabstop=atoi(val);if(cfg->active.tabstop<1)cfg->active.tabstop=8;break;
                                case 'S':if(!val)die("-S needs argument");scopy(cfg->sep, val, MAX_DELIM);break;
                                case 'g':if(!val)die("-g needs argument");scopy(cfg->grep_pat, val, MAX_LINE);break;
                                case 'v':if(!val)die("-v needs argument");scopy(cfg->vgrep_pat, val, MAX_LINE);break;
                                default:die("unknown flag -%c (try -h)",flag);
                        }
                } else {
                        if(*nfiles>=cap){cap*=2;*files=xrealloc(*files,cap*sizeof(char*));}
                        (*files)[(*nfiles)++]=a;
                }
        }
}

int main(int argc, char **argv){
        /* Allocate Config on heap - it embeds MAX_PRESETS Preset structs
           each containing a regex_t; together they exceed the default stack. */
        Config *cfg=xmalloc(sizeof(Config));
        config_init(cfg); load_builtins(cfg);
        char *cfgpath=default_config_path();
        if(cfgpath)load_config_file(cfgpath,cfg);

        char **files=NULL; int nfiles=0;
        parse_args(argc,argv,cfg,&files,&nfiles);

        /* compile regex if needed */
        if(!cfg->table_mode&&cfg->active.delim_type==DELIM_REGEX)
                if(!preset_compile(&cfg->active))
                        die("cannot compile regex: %s",cfg->active.delim);

        /* filter regexes */
        regex_t grep_re,vgrep_re;
        int hg=(cfg->grep_pat[0]!='\0'), hv=(cfg->vgrep_pat[0]!='\0');
        if(hg&&regcomp(&grep_re, cfg->grep_pat, REG_EXTENDED|REG_NOSUB)!=0)die("bad -g pattern");
        if(hv&&regcomp(&vgrep_re,cfg->vgrep_pat,REG_EXTENDED|REG_NOSUB)!=0)die("bad -v pattern");

        /* read input */
        int nraw=0; char **raw=NULL;
        if(cfg->json_in){
                size_t bsz=0,bcap=65536; char *blob=xmalloc(bcap); size_t nr;
                while((nr=fread(blob+bsz,1,bcap-bsz-1,stdin))>0){
                        bsz+=nr;if(bsz>=bcap-1){bcap*=2;blob=xrealloc(blob,bcap);}}
                blob[bsz]='\0'; raw=parse_json_array(blob,&nraw); free(blob);
        } else if(!nfiles){
                raw=read_lines_f(stdin,&nraw);
        } else {
                int cap=4096; raw=xmalloc(cap*sizeof(char*));
                for(int fi=0;fi<nfiles;fi++){
                        FILE *f=fopen(files[fi],"r");
                        if(!f)die("cannot open %s: %s",files[fi],strerror(errno));
                        int cnt=0; char **ch=read_lines_f(f,&cnt); fclose(f);
                        if(nraw+cnt>=cap){while(nraw+cnt>=cap)cap*=2;raw=xrealloc(raw,cap*sizeof(char*));}
                        memcpy(raw+nraw,ch,cnt*sizeof(char*)); nraw+=cnt; free(ch);
                }
        }

        /* build lines */
        Line *lines=xmalloc(nraw*sizeof(Line)); memset(lines,0,nraw*sizeof(Line));
        for(int i=0;i<nraw;i++){
                lines[i].raw=raw[i];
                if(hg&&regexec(&grep_re, raw[i],0,NULL,0)!=0){lines[i].filtered=1;continue;}
                if(hv&&regexec(&vgrep_re,raw[i],0,NULL,0)==0){lines[i].filtered=1;continue;}
                lines[i].split=cfg->table_mode?do_split_ws(raw[i]):do_split(raw[i],&cfg->active);
        }
        if(hg) regfree(&grep_re);
        if(hv) regfree(&vgrep_re);

        /* capture for JSON output */
        FILE *saved=NULL; char *obuf=NULL; size_t osz=0;
        if(cfg->json_out){
                saved=stdout;
                FILE *mem=open_memstream(&obuf,&osz);
                if(!mem) die("open_memstream");
                stdout=mem;
        }

        /* align */
        if(cfg->table_mode)                         align_table(lines,nraw,cfg);
        else if(cfg->active.nth_mode==NTH_ALT)      align_alt(lines,nraw,cfg);
        else if(cfg->active.nth_mode==NTH_ALL)      align_star(lines,nraw,cfg);
        else                                        align_single(lines,nraw,cfg);

        /* json out */
        if(cfg->json_out){
                fclose(stdout); stdout=saved;
                putchar('['); char *p=obuf; int first=1;
                while(*p){
                        char *nl=strchr(p,'\n'); size_t len=nl?(size_t)(nl-p):strlen(p);
                        char *ln=xstrndup(p,len);
                        if(!first) putchar(',');
                        putchar('\n');
                        json_str(ln);
                        free(ln);
                        first=0; p+=len+(nl?1:0); if(!nl)break;
                }
                puts("\n]"); free(obuf);
        }

        /* cleanup */
        for(int i=0;i<nraw;i++){split_free(&lines[i].split);free(raw[i]);}
        free(lines); free(raw); free(files);
        if(cfg->active.delim_compiled)regfree(&cfg->active.delim_re);
        free(cfg);
        return 0;
}
