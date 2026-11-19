/* Load the generated PE in a Linux process and exercise its real x64 code
   against a minimal Win32 API shim. This is not a Windows desktop test. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ABI __attribute__((ms_abi))
typedef uint16_t W;
typedef uintptr_t H;
typedef intptr_t L;
static const char *old_path, *new_path, *output_path;
static L (__attribute__((ms_abi)) *wnd)(H,uint32_t,H,L);
static int edits;
static uint32_t last_error;
static int drawn;
static int folder_picks;
static unsigned folder_flags;
static char selected_path[2][4096];

static void narrow(const W *in, char *out) { while ((*out++ = (char)*in++)); }
static void widen(const char *in, W *out) { while ((*out++ = (unsigned char)*in++)); }
static uint32_t u32(const uint8_t *p) { uint32_t n; memcpy(&n,p,4); return n; }
static uint64_t u64(const uint8_t *p) { uint64_t n; memcpy(&n,p,8); return n; }
static uint16_t u16(const uint8_t *p) { uint16_t n; memcpy(&n,p,2); return n; }

void *ABI mock_malloc(size_t n) { return malloc(n); }
void *ABI mock_realloc(void *p,size_t n) { return realloc(p,n); }
void ABI mock_free(void *p) { free(p); }
void *ABI mock_memcpy(void *a,const void *b,size_t n) { return memcpy(a,b,n); }
int ABI mock_memcmp(const void *a,const void *b,size_t n) { return memcmp(a,b,n); }
void *ABI mock_memset(void *a,int b,size_t n) { return memset(a,b,n); }
size_t ABI mock_strlen(const char *s) { return strlen(s); }
int ABI mock_strcmp(const char *a,const char *b) { return strcmp(a,b); }
size_t ABI mock_fread(void *b,size_t m,size_t n,FILE *f) {return fread(b,m,n,f);}
size_t ABI mock_fwrite(const void *b,size_t m,size_t n,FILE *f) {return fwrite(b,m,n,f);}
int ABI mock_fclose(FILE *f) {return fclose(f);}
int ABI mock_ferror(FILE *f) {return ferror(f);}
FILE *ABI mock__wfopen(const W *path,const W *mode) {
    char p[4096], m[8]; narrow(path,p); narrow(mode,m); return fopen(p,m);
}
int ABI mock_MultiByteToWideChar(int page,int flags,const char *src,int n,W *dst,int cap) {
    (void)page; (void)flags;
    if(dst) {assert(cap>=n); for(int i=0;i<n;i++) dst[i]=(unsigned char)src[i];}
    return n;
}
int ABI mock_WideCharToMultiByte(int page,int flags,const W *src,int n,char *dst,int cap,const char *x,int *y) {
    (void)page; (void)flags; (void)x; (void)y;
    if(dst) {assert(cap>=n); for(int i=0;i<n;i++) dst[i]=(char)src[i];}
    return n;
}
uint32_t ABI mock_GetFileAttributesW(const W *path) {
    char p[4096]; narrow(path,p); struct stat a;
    return stat(p,&a) ? UINT32_MAX : S_ISDIR(a.st_mode) ? 16 : 0;
}
typedef struct { DIR *directory; char path[1024]; } MockFind;
int ABI mock_FindNextFileW(MockFind *search, uint8_t *record) {
    struct dirent *entry = readdir(search->directory);
    if(!entry) {last_error=18; return 0;}
    char path[1350];
    snprintf(path,sizeof(path),"%s/%s",search->path,entry->d_name);
    struct stat stats;
    assert(lstat(path,&stats)==0);
    uint32_t mode=S_ISDIR(stats.st_mode)?16:S_ISLNK(stats.st_mode)?1024:0;
    memcpy(record,&mode,4);
    widen(entry->d_name,(W *)(record+44));
    return 1;
}
MockFind *ABI mock_FindFirstFileW(const W *pattern,uint8_t *record) {
    MockFind *search=malloc(sizeof(*search)); assert(search);
    narrow(pattern,search->path);
    *strrchr(search->path,'*')=0;
    search->directory=opendir(search->path);
    if(search->directory && mock_FindNextFileW(search,record)) return search;
    if(search->directory) closedir(search->directory);
    free(search);
    last_error=2;
    return (MockFind *)-1;
}
int ABI mock_FindClose(MockFind *search) {closedir(search->directory);free(search);return 1;}
uint32_t ABI mock_GetLastError(void) {return last_error;}
unsigned ABI mock_GetTempFileNameW(const W *dir,const W *prefix,unsigned number,W *name) {
    (void)prefix; (void)number; char p[4096]; narrow(dir,p); strcat(p,"/FNXXXXXX");
    int f=mkstemp(p); if(f<0) return 0; close(f); widen(p,name); return 1;
}
int ABI mock_MoveFileExW(const W *old,const W *newer,unsigned flags) {
    (void)flags; char a[4096],b[4096]; narrow(old,a); narrow(newer,b); return rename(a,b)==0;
}
int ABI mock_DeleteFileW(const W *name) { char p[4096]; narrow(name,p); return unlink(p)==0; }
void ABI mock_ExitProcess(unsigned status) { exit((int)status); }
H ABI mock_GetModuleHandleW(const W *name) { (void)name; return 100; }
int ABI mock_CoInitializeEx(void *ignored,unsigned flags) { (void)ignored;(void)flags;return 0; }
void ABI mock_CoUninitialize(void) {}
void ABI mock_CoTaskMemFree(void *memory) {free(memory);}
typedef struct MockCom { void **vtable; } MockCom;
static unsigned ABI release_mock(MockCom *object) {assert(object);return 0;}
static int ABI show_picker(MockCom *object,H hwnd) {
    assert(object && hwnd==1);
    assert((folder_flags & 0x860)==0x860);
    return 0;
}
static int ABI set_picker_options(MockCom *object,unsigned flags) {
    assert(object);
    folder_flags=flags;
    return 0;
}
static int ABI get_picker_options(MockCom *object,unsigned *flags) {
    assert(object);
    *flags=2;
    return 0;
}
static int ABI set_picker_title(MockCom *object,const W *title) {
    assert(object && title[0]);
    return 0;
}
static int ABI get_item_path(MockCom *object,unsigned type,W **name) {
    assert(object && type==0x80058000u && folder_picks<2);
    const char *path=folder_picks++ ? new_path : old_path;
    *name=malloc((strlen(path)+1)*sizeof(**name));
    widen(path,*name);
    return 0;
}
static int ABI get_picker_result(MockCom *object,MockCom **item) {
    static void *functions[]={ [2]=release_mock,[5]=get_item_path };
    static MockCom shell_item={functions};
    assert(object);
    *item=&shell_item;
    return 0;
}
int ABI mock_CoCreateInstance(const uint8_t *cls,void *outer,unsigned context,
                            const uint8_t *interface,MockCom **object) {
    static void *functions[]={
        [2]=release_mock,[3]=show_picker,[9]=set_picker_options,
        [10]=get_picker_options,[17]=set_picker_title,[20]=get_picker_result,
    };
    static MockCom picker={functions};
    assert(u32(cls)==0xdc1c5a9c && u32(interface)==0xd57c7288 && !outer && context==1);
    *object=&picker;
    return 0;
}
H ABI mock_LoadCursorW(H owner,const W *name) {(void)owner;(void)name;return 1;}
H ABI mock_LoadImageW(H owner,const W *name,unsigned kind,int w,int h,unsigned flags) {
    assert(owner==100 && (H)name==1 && kind==1 && w==h && !flags);
    assert(w==64 || w==16);
    return (H)w;
}
int ABI mock_DestroyIcon(H icon) {assert(icon==64 || icon==16);return 1;}
H ABI mock_CreateSolidBrush(unsigned color) {return color+1;}
int ABI mock_DeleteObject(H object) {assert(object);return 1;}
H ABI mock_BeginPaint(H hwnd,void *paint) {assert(hwnd==1); memset(paint,0,72); return 2;}
int ABI mock_EndPaint(H hwnd,const void *paint) {assert(hwnd==1 && paint);return 1;}
int ABI mock_DrawIconEx(H dc,int x,int y,H icon,int w,int h,unsigned step,H brush,unsigned flags) {
    assert(dc==2 && x>0 && y>0 && icon==64 && w==62 && h==62 && !step && !brush && flags==3);
    drawn=1;
    return 1;
}
int ABI mock_FillRect(H dc,const int *rect,H brush) {
    assert(dc==2 && rect[2]>rect[0] && rect[3]>rect[1] && brush);
    return 1;
}
H ABI mock_SelectObject(H dc,H object) {assert(dc==2 && object);return 1;}
int ABI mock_SetBkMode(H dc,int mode) {assert(dc==2 && mode==1); return 1;}
unsigned ABI mock_SetTextColor(H dc,unsigned color) {assert(dc==2);return color;}
int ABI mock_DrawTextW(H dc,const W *text,int len,int *rect,unsigned flags) {
    assert(dc==2 && text[0] && len==-1 && rect && (flags & 0x20));
    return 1;
}
int ABI mock_DrawFocusRect(H dc,const int *rect) {assert(dc==2 && rect);return 1;}
static int after_only, after_button_created;
unsigned short ABI mock_RegisterClassExW(const uint8_t *data) {
    assert(u32(data)==80);
    assert(u64(data+32)==64 && u64(data+72)==16);
    uintptr_t address=u64(data+8);
    memcpy(&wnd,&address,sizeof(wnd));
    return 1;
}
H ABI mock_CreateWindowExW(unsigned ext,const W *cls,const W *name,unsigned style,int x,int y,int w,int h,
                          H parent,H menu,H module,void *data) {
    (void)ext;(void)name;(void)style;(void)x;(void)y;(void)w;(void)h;(void)menu;(void)module;(void)data;
    if(!parent) { wnd(1,1,0,0); return 1; }
    if (menu==107) {
        static const W expected[] = {0x5bfc,0x51fa,0x65b0,0x7248,' ','J','S','O','N',0};
        assert(cls[0]=='B' && !memcmp(name,expected,sizeof(expected)) && x+w<=700 && y+h<=455);
        after_button_created=1;
    }
    return cls[0]=='E' ? 10+(H)edits++ : 90;
}
H ABI mock_CreateFontW(void) {return 1;}
L ABI mock_SendMessageW(H c,unsigned m,H w,L l) {(void)c;(void)m;(void)w;(void)l;return 0;}
int ABI mock_ShowWindow(H hwnd,int how) {(void)hwnd;(void)how;return 1;}
int ABI mock_UpdateWindow(H hwnd) {
    wnd(hwnd,15,0,0);
    assert(drawn);
    return 1;
}
H ABI mock_SetFocus(H hwnd) {return hwnd;}
int ABI mock_EnableWindow(H hwnd,int enabled) {(void)hwnd;(void)enabled;return 1;}
int ABI mock_SetWindowTextW(H hwnd,const W *text) {
    if (hwnd==10 || hwnd==11) narrow(text,selected_path[hwnd-10]);
    return 1;
}
int ABI mock_GetWindowTextW(H hwnd,W *result,int cap) {
    const char *path=folder_picks ? selected_path[hwnd-10] : hwnd==10 ? old_path : new_path;
    assert((int)strlen(path)<cap);
    widen(path,result); return (int)strlen(path);
}
int ABI mock_GetSaveFileNameW(uint8_t *dialog) {
    assert(u32(dialog)==152);
    if (after_only) {
        static const W expected[] = {0x53d8,0x66f4,0x51fd,0x6570,'-',0x4ec5,0x65b0,0x7248,'.','j','s','o','n',0};
        assert(!memcmp((const W *)(uintptr_t)u64(dialog+48),expected,sizeof(expected)));
    }
    widen(output_path,(W *)(uintptr_t)u64(dialog+48));
    return 1;
}
int ABI mock_MessageBoxW(H hwnd,const W *text,const W *title,unsigned flags) {
    (void)hwnd;(void)title;(void)flags;
    if(text[0]==0) abort();
    return 7;
}
int ABI mock_GetMessageW(void *m,H hwnd,unsigned min,unsigned max) {
    static int seen; (void)m;(void)hwnd;(void)min;(void)max; return !seen++;
}
int ABI mock_IsDialogMessageW(H hwnd,void *m) {(void)hwnd;(void)m;return 0;}
int ABI mock_TranslateMessage(void *m) {(void)m;return 1;}
L ABI mock_DispatchMessageW(void *m) {
    (void)m;
    struct stat info;
    if (!stat(old_path,&info) && S_ISDIR(info.st_mode)) {
        wnd(1,0x0111,102,0);
        wnd(1,0x0111,104,0);
        assert(folder_picks==2 && !strcmp(selected_path[0],old_path) && !strcmp(selected_path[1],new_path));
    }
    if (after_only) assert(after_button_created);
    return wnd(1,0x0111,after_only ? 107 : strstr(output_path,".json") ? 106 : 105,0);
}
void ABI mock_unexpected(void) { fputs("Unexpected PE import call\n",stderr); abort(); }

int main(int argc,char **argv)
{
    assert(argc==5 || (argc==6 && !strcmp(argv[5],"after")));
    after_only=argc==6;
    old_path=argv[2]; new_path=argv[3]; output_path=argv[4];
    FILE *f=fopen(argv[1],"rb"); assert(f);
    assert(fseek(f,0,SEEK_END)==0); long size=ftell(f); rewind(f);
    uint8_t *raw=malloc((size_t)size); assert(fread(raw,1,(size_t)size,f)==(size_t)size); fclose(f);
    const uint8_t *pe=raw+u32(raw+0x3c), *opt=pe+24, *sections=opt+u16(pe+20);
    assert(!memcmp(pe,"PE\0\0",4) && u16(opt)==0x20b && u16(opt+68)==2);
    size_t image_size=u32(opt+56);
    uint8_t *base=mmap(NULL,image_size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(base!=MAP_FAILED);
    for(size_t i=0;i<u16(pe+6);i++) {
        const uint8_t *s=sections+i*40;
        if(u32(s+16)) memcpy(base+u32(s+12),raw+u32(s+20),u32(s+16));
    }
    uint8_t *rel=base+u32(opt+112+5*8), *rel_end=rel+u32(opt+116+5*8);
    while(rel<rel_end) {
        uint32_t page=u32(rel), block=u32(rel+4);
        assert(block>=8);
        for(uint32_t i=8;i<block;i+=2) {
            uint16_t slot=u16(rel+i);
            if(!slot) continue;
            assert(slot>>12==10);
            uint8_t *address=base+page+(slot&0xfff);
            uint64_t v=u64(address)+(uintptr_t)base-u64(opt+24);
            memcpy(address,&v,8);
        }
        rel+=block;
    }
    for(uint8_t *d=base+u32(opt+120);u32(d+12);d+=20) {
        for(uint8_t *slot=base+u32(d+16);u64(slot);slot+=8) {
            const char *name=(const char *)(base+u64(slot)+2);
            char mock[160]; snprintf(mock,sizeof(mock),"mock_%s",name);
            void *replacement=dlsym(RTLD_DEFAULT,mock);
            if(!replacement) replacement=dlsym(RTLD_DEFAULT,"mock_unexpected");
            assert(replacement);
            memcpy(slot,&replacement,8);
        }
    }
    typedef void (__attribute__((ms_abi)) *Entry)(void);
    Entry entry=(Entry)(base+u32(opt+16));
    entry();
    abort();
}
