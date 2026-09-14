#include <windows.h>
#include "PlaythroughSession.h"
#include <map>
#include <sstream>
#include <vector>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")

namespace PlaythroughSession {
namespace {
    std::string NewId() {
        HCRYPTPROV provider = 0; BYTE bytes[16];
        if (!CryptAcquireContext(&provider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return "";
        const BOOL ok = CryptGenRandom(provider, sizeof(bytes), bytes);
        CryptReleaseContext(provider, 0);
        if (!ok) return "";
        const char* hex = "0123456789abcdef"; std::string id;
        for (int i=0;i<16;++i) { id += hex[bytes[i]>>4]; id += hex[bytes[i]&15]; }
        return id;
    }
    struct State {
        CRITICAL_SECTION mutex;
        DWORD tls; unsigned long generation, started;
        bool ready, isNew, connectedNotice;
        std::string character, receipt, client, notice;
        State() : generation(1), started(0), ready(false), isNew(false), connectedNotice(false) {
            InitializeCriticalSection(&mutex); tls = TlsAlloc(); client = NewId();
        }
    };
    BOOL CALLBACK Init(PINIT_ONCE, PVOID, PVOID* context) { *context = new State(); return TRUE; }
    State& Get() {
        static INIT_ONCE once = INIT_ONCE_STATIC_INIT; PVOID context = NULL;
        InitOnceExecuteOnce(&once, Init, NULL, &context); return *static_cast<State*>(context);
    }
    struct Lock { Lock() { EnterCriticalSection(&Get().mutex); } ~Lock() { LeaveCriticalSection(&Get().mutex); } };
    std::string Escape(const std::string& value) {
        std::string out; const char* hex="0123456789abcdef";
        for (size_t i=0;i<value.size();++i) {
            unsigned char c=value[i];
            if (c=='"' || c=='\\') { out+='\\'; out+=c; }
            else if (c<32) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
            else out+=c;
        }
        return out;
    }
    void Space(const std::string& s, size_t& p) { while(p<s.size() && (s[p]==' '||s[p]=='\r'||s[p]=='\n'||s[p]=='\t')) ++p; }
    // The handshake is a flat JSON object. Reject duplicate fields, wrong types and malformed responses.
    bool String(const std::string& s, size_t& p, std::string& out) {
        if(p>=s.size()||s[p++]!='"') return false;
        while(p<s.size()) {
            unsigned char c=s[p++]; if(c=='"') return true; if(c<32) return false;
            if(c!='\\') { out+=c; continue; }
            if(p>=s.size()) return false; char e=s[p++];
            if(e=='"'||e=='\\'||e=='/') out+=e;
            else if(e=='n') out+='\n'; else if(e=='r') out+='\r'; else if(e=='t') out+='\t';
            else if(e=='b') out+='\b'; else if(e=='f') out+='\f';
            else if(e=='u') {
                wchar_t wide[2]={0,0}; int count=1;
                for(int n=0;n<count;++n) {
                    if(p+4>s.size()) return false;
                    for(int j=0;j<4;++j) { char h=s[p++]; int v=h>='0'&&h<='9'?h-'0':h>='a'&&h<='f'?h-'a'+10:h>='A'&&h<='F'?h-'A'+10:-1; if(v<0)return false; wide[n]=(wide[n]<<4)|v; }
                    if(n==0 && wide[0]>=0xd800 && wide[0]<=0xdbff) { if(s.substr(p,2)!="\\u")return false;p+=2;count=2; }
                }
                char utf8[8]; int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,count,utf8,8,NULL,NULL);
                if(!bytes)return false;out.append(utf8,bytes);
            } else return false;
        }
        return false;
    }
    bool Parse(const std::string& s, std::map<std::string,std::string>& fields) {
        if(s.size()>16384)return false; size_t p=0;Space(s,p);if(p>=s.size()||s[p++]!='{')return false;
        for(;;) {
            Space(s,p);if(p<s.size()&&s[p]=='}'){++p;break;}
            std::string key,value;if(!String(s,p,key)||fields.count(key))return false;Space(s,p);
            if(p>=s.size()||s[p++]!=':')return false;Space(s,p);
            if(p<s.size()&&s[p]=='"'){if(!String(s,p,value))return false;value="s"+value;}
            else {size_t start=p;while(p<s.size()&&s[p]!=','&&s[p]!='}'&&s[p]!=' '&&s[p]!='\r'&&s[p]!='\n')++p;value="l"+s.substr(start,p-start);}
            fields[key]=value;Space(s,p);if(p<s.size()&&s[p]==','){++p;Space(s,p);if(p>=s.size()||s[p]=='}')return false;continue;}
            if(p<s.size()&&s[p]=='}'){++p;break;}return false;
        }
        Space(s,p);return p==s.size();
    }
    struct Request { unsigned long epoch; std::string body; };
    DWORD WINAPI Run(PVOID data) {
        Request request=*static_cast<Request*>(data);delete static_cast<Request*>(data);Scope scope(request.epoch);
        const ULONGLONG deadline=GetTickCount64()+30000; bool transportRetried=false, accepted=false;
        std::map<std::string,std::string> fields;
        try {
            for(;;) {
                if(request.epoch!=Generation())return 0;
                unsigned long status=0;std::string body=Transport(request.body,status);fields.clear();
                if(status==404){fields["status"]="sunsupported";accepted=true;break;}
                bool valid=Parse(body,fields);
                if(status==200&&valid&&fields["ok"]=="ltrue") {
                    accepted=fields["status"]=="soff" || (fields["status"]=="sready" && fields["token"].size()==33 && fields["character_id"].size()==33 && ValidId(fields["token"].substr(1)) && ValidId(fields["character_id"].substr(1)) && fields["token"][0]=='s' && fields["character_id"][0]=='s');break;
                }
                if(status==0&&!transportRetried){transportRetried=true;continue;}
                if(status!=503||!valid||fields["status"]!="sbusy"||fields["ok"]!="lfalse"||GetTickCount64()>=deadline)break;
                for(int i=0;i<10;++i){if(request.epoch!=Generation())return 0;Sleep(100);}
            }
        } catch(...) { fields.clear(); }
        Lock lock;State& state=Get();if(request.epoch!=state.generation)return 0;
        if(accepted) {
            if(fields["status"]=="sready") { state.receipt=fields["token"].substr(1);state.character=fields["character_id"].substr(1);state.isNew=false; }
            state.ready=true;
        }
        if(fields["message"].size()>1&&fields["message"][0]=='s')state.notice=fields["message"].substr(1);
        else if(!accepted)state.notice="Playthrough unavailable. Open Playthrough Saves, then reload this save.";
        else if(!state.connectedNotice)state.notice="Connected";
        if(accepted)state.connectedNotice=true;
        return 0;
    }
}
bool ValidId(const std::string& id){return id.size()==32&&id.find_first_not_of("0123456789abcdef")==std::string::npos;}
unsigned long Generation(){Lock lock;return Get().generation;}
unsigned long Context(){DWORD value=static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(TlsGetValue(Get().tls)));return value?value:Generation();}
Scope::Scope(unsigned long epoch){previous=static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(TlsGetValue(Get().tls)));TlsSetValue(Get().tls,reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(epoch)));}
Scope::~Scope(){TlsSetValue(Get().tls,reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(previous)));}
bool Allowed(unsigned long epoch){Lock lock;return Get().ready&&epoch==Get().generation;}
std::wstring Headers(unsigned long epoch){Lock lock;std::string token=Allowed(epoch)?(Get().receipt.empty()?"legacy":Get().receipt):"blocked";return L"\r\nX-DIALECTIC-Playthrough: "+std::wstring(token.begin(),token.end())+L"\r\n";}
void BeginLoad(bool newGame){Lock lock;State& s=Get();++s.generation;s.ready=false;s.receipt.clear();s.notice.clear();s.character=newGame?NewId():"";s.isNew=newGame;}
std::string Character(){Lock lock;return Get().character;}
bool NewCharacter(){Lock lock;return Get().isNew;}
void RestoreCharacter(const std::string& id,bool isNew){if(!ValidId(id))return;Lock lock;Get().character=id;Get().isNew=isNew;}
bool TakeNotice(std::string& message){Lock lock;message.swap(Get().notice);return !message.empty();}
void Connect(const std::string& name,long long gamets,const std::string& members){
    Lock lock;State& s=Get();if(s.started==s.generation||name.empty()||gamets<=0)return;
    if(s.client.empty()||(s.isNew&&s.character.empty())){s.notice="Could not identify this save. Reload the game.";s.started=s.generation;return;}
    s.started=s.generation;std::ostringstream json;
    json<<"{\"client_id\":\""<<s.client<<"\",\"load_id\":"<<s.generation<<",\"character_id\":\""<<s.character<<"\",\"new_game\":"<<(s.isNew?"true":"false")<<",\"player_name\":\""<<Escape(name)<<"\",\"gamets\":"<<gamets<<",\"player_members\":"<<members<<"}";
    Request* request=new Request();request->epoch=s.generation;request->body=json.str();
    HANDLE thread=CreateThread(NULL,0,Run,request,0,NULL);if(thread)CloseHandle(thread);else{delete request;s.notice="Could not connect this playthrough. Reload the save.";}
}
}
