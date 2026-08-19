#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstdio>
#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"comctl32.lib")
namespace fs=std::filesystem;
constexpr UINT WM_LOG=WM_APP+1,WM_DONE=WM_APP+2;
HWND g_main{},g_log{},g_button{},g_status{},g_progress{};

std::wstring Wide(const std::string&s){if(s.empty())return{};int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);std::wstring r(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n);return r;}
std::string Utf8(const std::wstring&s){if(s.empty())return{};int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);std::string r(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr);return r;}
std::string JsonEscape(const std::string&s){std::string r;r.reserve(s.size()+16);for(unsigned char c:s){switch(c){case '\\':r+="\\\\";break;case '"':r+="\\\"";break;case '\b':r+="\\b";break;case '\f':r+="\\f";break;case '\n':r+="\\n";break;case '\r':r+="\\r";break;case '\t':r+="\\t";break;default:if(c<0x20){char b[7];sprintf_s(b,"\\u%04x",c);r+=b;}else r+=(char)c;}}return r;}
void Log(const std::wstring&s){PostMessage(g_main,WM_LOG,0,(LPARAM)new std::wstring(s));}
struct Internet{HINTERNET h{};explicit Internet(HINTERNET v=nullptr):h(v){}~Internet(){if(h)WinHttpCloseHandle(h);}operator HINTERNET()const{return h;}};

std::vector<BYTE> Get(const std::wstring&url,const std::wstring&proxy=L"",DWORD timeout=20000){
 URL_COMPONENTS p{sizeof(p)};wchar_t host[256]{},path[2048]{},extra[2048]{};p.lpszHostName=host;p.dwHostNameLength=256;p.lpszUrlPath=path;p.dwUrlPathLength=2048;p.lpszExtraInfo=extra;p.dwExtraInfoLength=2048;
 if(!WinHttpCrackUrl(url.c_str(),0,0,&p))throw std::runtime_error("URL invalida");
 Internet session(WinHttpOpen(L"DiscordRefreshProxy/2.0",proxy.empty()?WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY:WINHTTP_ACCESS_TYPE_NAMED_PROXY,proxy.empty()?WINHTTP_NO_PROXY_NAME:proxy.c_str(),WINHTTP_NO_PROXY_BYPASS,0));
 if(!session.h)throw std::runtime_error("WinHTTP indisponivel");WinHttpSetTimeouts(session,timeout,timeout,timeout,timeout);
 Internet connection(WinHttpConnect(session,host,p.nPort,0));DWORD flags=p.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0;std::wstring requestPath=std::wstring(path)+extra;
 Internet request(WinHttpOpenRequest(connection,L"GET",requestPath.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,flags));
 if(!request.h||!WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request,nullptr))throw std::runtime_error("Falha na requisicao HTTPS");
 DWORD status{},size=sizeof(status);WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
 if(status<200||status>=400)throw std::runtime_error("Servidor respondeu HTTP "+std::to_string(status));
 std::vector<BYTE> out;for(;;){DWORD available{};if(!WinHttpQueryDataAvailable(request,&available))throw std::runtime_error("Falha ao ler resposta");if(!available)break;size_t old=out.size();out.resize(old+available);DWORD read{};if(!WinHttpReadData(request,out.data()+old,available,&read))throw std::runtime_error("Falha no download");out.resize(old+read);}return out;
}
void Save(const fs::path&p,const std::vector<BYTE>&b){std::ofstream f(p,std::ios::binary);if(!f)throw std::runtime_error("Nao foi possivel criar arquivo");f.write((const char*)b.data(),(std::streamsize)b.size());}
std::string Sha256(const fs::path&p){
 BCRYPT_ALG_HANDLE alg{};BCRYPT_HASH_HANDLE hash{};DWORD objectSize{},got{};
 if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)||BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,(PUCHAR)&objectSize,sizeof(objectSize),&got,0))throw std::runtime_error("SHA-256 indisponivel");
 std::vector<BYTE> object(objectSize),digest(32),buffer(1<<20);if(BCryptCreateHash(alg,&hash,object.data(),objectSize,nullptr,0,0))throw std::runtime_error("Falha SHA-256");
 std::ifstream f(p,std::ios::binary);while(f){f.read((char*)buffer.data(),buffer.size());if(f.gcount())BCryptHashData(hash,buffer.data(),(ULONG)f.gcount(),0);}if(BCryptFinishHash(hash,digest.data(),32,0))throw std::runtime_error("Falha SHA-256");
 BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);const char*hex="0123456789abcdef";std::string r;for(BYTE x:digest){r+=hex[x>>4];r+=hex[x&15];}return r;
}
DWORD Hidden(const std::wstring&cmd,DWORD wait,PROCESS_INFORMATION*keep=nullptr){STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;std::vector<wchar_t> c(cmd.begin(),cmd.end());c.push_back(0);if(!CreateProcessW(nullptr,c.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))throw std::runtime_error("Nao foi possivel iniciar componente");if(keep){*keep=pi;return STILL_ACTIVE;}WaitForSingleObject(pi.hProcess,wait);DWORD code{};GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code;}
fs::path Root(){wchar_t b[MAX_PATH]{};if(!GetEnvironmentVariableW(L"LOCALAPPDATA",b,MAX_PATH))throw std::runtime_error("LOCALAPPDATA indisponivel");return fs::path(b)/L"DiscordRefreshProxy";}
std::string Value(const std::string&t,const std::string&m,size_t start){size_t a=t.find(m,start);if(a==std::string::npos)return{};a+=m.size();size_t e=t.find('"',a);return e==std::string::npos?std::string{}:t.substr(a,e-a);}

fs::path EnsureSingBox(){
 fs::path root=Root(),pointer=root/L"current.txt";if(fs::exists(pointer)){std::ifstream f(pointer);std::string v;std::getline(f,v);fs::path e=root/Wide(v)/L"sing-box.exe";if(fs::exists(e)){Log(L"Componente de rede ja instalado.");return e;}}
 Log(L"Consultando o ultimo release estavel do sing-box...");auto bytes=Get(L"https://api.github.com/repos/SagerNet/sing-box/releases/latest");std::string json(bytes.begin(),bytes.end());
 std::string tag=Value(json,"\"tag_name\":\"",0);if(tag.empty())tag=Value(json,"\"tag_name\": \"",0);if(tag.empty())throw std::runtime_error("Release oficial sem versao");std::string version=tag[0]=='v'?tag.substr(1):tag;
#ifdef _M_ARM64
 std::string arch="arm64";
#else
 std::string arch="amd64";
#endif
 std::string name="sing-box-"+version+"-windows-"+arch+".zip";size_t asset=json.find("\"name\":\""+name+"\"");if(asset==std::string::npos)asset=json.find("\"name\": \""+name+"\"");if(asset==std::string::npos)throw std::runtime_error("Pacote Windows ausente no release");
 std::string digest=Value(json,"\"digest\":\"sha256:",asset);if(digest.empty())digest=Value(json,"\"digest\": \"sha256:",asset);std::string url=Value(json,"\"browser_download_url\":\"",asset);if(url.empty())url=Value(json,"\"browser_download_url\": \"",asset);if(digest.size()!=64||url.empty())throw std::runtime_error("Release sem URL ou SHA-256");
 fs::path temp=fs::temp_directory_path()/(L"discord-refresh-"+std::to_wstring(GetTickCount64()));fs::create_directories(temp);fs::path zip=temp/Wide(name);
 try{Log(L"Baixando sing-box oficial "+Wide(tag)+L"...");Save(zip,Get(Wide(url),L"",120000));std::transform(digest.begin(),digest.end(),digest.begin(),::tolower);if(Sha256(zip)!=digest)throw std::runtime_error("Checksum do sing-box nao confere");
  if(Hidden(L"tar.exe -xf \""+zip.wstring()+L"\" -C \""+temp.wstring()+L"\"",120000)!=0)throw std::runtime_error("Falha ao extrair pacote");fs::path found;for(auto&i:fs::recursive_directory_iterator(temp))if(i.path().filename()==L"sing-box.exe"){found=i.path();break;}if(found.empty())throw std::runtime_error("sing-box.exe ausente");
  fs::path target=root/Wide(version);fs::create_directories(target);fs::copy_file(found,target/L"sing-box.exe",fs::copy_options::overwrite_existing);fs::create_directories(root);std::ofstream(pointer)<<version;fs::remove_all(temp);Log(L"Componente instalado e checksum verificado.");return target/L"sing-box.exe";
 }catch(...){std::error_code ec;fs::remove_all(temp,ec);throw;}
}

struct Proxy{std::wstring host;int port;std::wstring country;};
Proxy FindProxy(){std::vector<std::pair<std::wstring,std::wstring>> countries={{L"US",L"Estados Unidos"},{L"CA",L"Canada"},{L"NL",L"Paises Baixos"},{L"DE",L"Alemanha"},{L"FR",L"Franca"},{L"GB",L"Reino Unido"},{L"JP",L"Japao"},{L"SG",L"Singapura"}};std::mt19937 rng((unsigned)GetTickCount());std::shuffle(countries.begin(),countries.end(),rng);std::regex re(R"((\d{1,3}(?:\.\d{1,3}){3}):(\d{2,5}))");
 for(auto&country:countries){Log(L"Buscando proxies em "+country.second+L"...");std::wstring uri=L"https://api.proxyscrape.com/v4/free-proxy-list/get?request=getproxies&protocol=http&country="+country.first+L"&timeout=5000&ssl=yes&anonymity=elite,anonymous&limit=80";std::string body;try{auto b=Get(uri);body.assign(b.begin(),b.end());}catch(...){continue;}std::vector<std::pair<std::string,int>> list;for(std::sregex_iterator i(body.begin(),body.end(),re),e;i!=e;++i)list.push_back({(*i)[1],std::stoi((*i)[2])});std::shuffle(list.begin(),list.end(),rng);int attempts=0;for(auto&p:list){if(++attempts>12)break;Log(L"  Testando "+Wide(p.first)+L":"+std::to_wstring(p.second)+L"...");try{Get(L"https://discord.com/api/v10/gateway",Wide(p.first)+L":"+std::to_wstring(p.second),7000);return{Wide(p.first),p.second,country.second};}catch(...){}}}throw std::runtime_error("Nenhum proxy publico funcional encontrado. Tente novamente.");}

fs::path Config(const Proxy&p){fs::path root=Root(),logs=root/L"logs";fs::create_directories(logs);fs::path path=root/(L"session-"+std::to_wstring(GetTickCount64())+L".json");std::ostringstream j;std::string logPath=JsonEscape(Utf8((logs/L"session.log").wstring()));std::string host=JsonEscape(Utf8(p.host));
 j<<"{\"log\":{\"level\":\"info\",\"output\":\""<<logPath<<"\",\"timestamp\":true},"
  <<"\"dns\":{\"servers\":[{\"type\":\"local\",\"tag\":\"dns-local\"}],\"final\":\"dns-local\",\"strategy\":\"prefer_ipv4\"},"
  <<"\"inbounds\":[{\"type\":\"tun\",\"tag\":\"tun-in\",\"interface_name\":\"DiscordRefresh\",\"address\":[\"172.20.0.1/30\"],\"mtu\":1408,\"auto_route\":true,\"strict_route\":true,\"stack\":\"mixed\"}],"
  <<"\"outbounds\":[{\"type\":\"direct\",\"tag\":\"direct\"},{\"type\":\"http\",\"tag\":\"temp-proxy\",\"server\":\""<<host<<"\",\"server_port\":"<<p.port<<"}],"
  <<"\"route\":{\"auto_detect_interface\":true,\"default_domain_resolver\":\"dns-local\",\"rules\":[{\"action\":\"sniff\"},{\"protocol\":\"dns\",\"action\":\"hijack-dns\"},{\"ip_is_private\":true,\"action\":\"route\",\"outbound\":\"direct\"},{\"process_name\":[\"Discord.exe\",\"DiscordCanary.exe\",\"DiscordPTB.exe\",\"DiscordDevelopment.exe\"],\"network\":\"tcp\",\"action\":\"route\",\"outbound\":\"temp-proxy\"}],\"final\":\"direct\"}}";
 std::ofstream file(path,std::ios::binary);file<<j.str();if(!file)throw std::runtime_error("Nao foi possivel gravar a configuracao");return path;}

BOOL CALLBACK DiscordEnum(HWND w,LPARAM out){if(!IsWindowVisible(w))return TRUE;DWORD pid{};GetWindowThreadProcessId(w,&pid);HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!h)return TRUE;wchar_t path[32768]{};DWORD n=32768;QueryFullProcessImageNameW(h,0,path,&n);CloseHandle(h);std::wstring name=fs::path(path).filename().wstring();std::transform(name.begin(),name.end(),name.begin(),::towlower);if(name==L"discord.exe"||name==L"discordcanary.exe"||name==L"discordptb.exe"||name==L"discorddevelopment.exe"){*(HWND*)out=w;return FALSE;}return TRUE;}
HWND Discord(){HWND w{};EnumWindows(DiscordEnum,(LPARAM)&w);return w;}
void Refresh(HWND w){ShowWindowAsync(w,SW_RESTORE);SetWindowPos(w,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);SetWindowPos(w,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);DWORD target=GetWindowThreadProcessId(w,nullptr),current=GetCurrentThreadId();BOOL attached=target!=current&&AttachThreadInput(current,target,TRUE);BringWindowToTop(w);SetForegroundWindow(w);SetFocus(w);Sleep(700);if(attached)AttachThreadInput(current,target,FALSE);if(GetForegroundWindow()==w){keybd_event(VK_CONTROL,0,0,0);keybd_event('R',0,0,0);Sleep(80);keybd_event('R',0,KEYEVENTF_KEYUP,0);keybd_event(VK_CONTROL,0,KEYEVENTF_KEYUP,0);}else{PostMessage(w,WM_KEYDOWN,VK_CONTROL,0);PostMessage(w,WM_KEYDOWN,'R',0);Sleep(80);PostMessage(w,WM_KEYUP,'R',0);PostMessage(w,WM_KEYUP,VK_CONTROL,0);}}

DWORD WINAPI Worker(LPVOID){PROCESS_INFORMATION tunnel{};fs::path config;try{HWND discord=Discord();if(!discord)throw std::runtime_error("Abra o Discord antes de continuar");Log(L"Discord encontrado.");fs::path sing=EnsureSingBox();Proxy proxy=FindProxy();Log(L"Proxy aprovado: "+proxy.host+L":"+std::to_wstring(proxy.port)+L" - "+proxy.country);config=Config(proxy);DWORD validation=Hidden(L"\""+sing.wstring()+L"\" check -c \""+config.wstring()+L"\"",30000);if(validation!=0)throw std::runtime_error("A configuracao temporaria foi rejeitada (codigo "+std::to_string(validation)+")");Hidden(L"\""+sing.wstring()+L"\" run -c \""+config.wstring()+L"\"",0,&tunnel);Sleep(2000);DWORD code{};GetExitCodeProcess(tunnel.hProcess,&code);if(code!=STILL_ACTIVE)throw std::runtime_error("Tunel temporario nao iniciou (codigo "+std::to_string(code)+")");Log(L"Tunel ativo. Enviando Ctrl+R...");Refresh(discord);Log(L"Discord recarregado. Mantendo o proxy por 45 segundos...");for(int remaining=45;remaining>0;remaining-=5){SetWindowTextW(g_status,(L"AGUARDE "+std::to_wstring(remaining)+L"s").c_str());Sleep(5000);GetExitCodeProcess(tunnel.hProcess,&code);if(code!=STILL_ACTIVE)throw std::runtime_error("O tunel encerrou antes de completar a recarga");}Log(L"Concluido usando saida em "+proxy.country+L".");PostMessage(g_main,WM_DONE,TRUE,0);}catch(const std::exception&e){Log(L"ERRO: "+Wide(e.what()));PostMessage(g_main,WM_DONE,FALSE,0);}if(tunnel.hProcess){TerminateProcess(tunnel.hProcess,0);WaitForSingleObject(tunnel.hProcess,5000);CloseHandle(tunnel.hThread);CloseHandle(tunnel.hProcess);}if(!config.empty()){std::error_code ec;fs::remove(config,ec);}return 0;}

LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM wp,LPARAM lp){switch(m){case WM_CREATE:{HFONT font=CreateFontW(18,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");CreateWindowW(L"STATIC",L"Discord Refresh Proxy",WS_CHILD|WS_VISIBLE,20,18,400,34,w,nullptr,nullptr,nullptr);CreateWindowW(L"STATIC",L"Proxy temporario apenas durante a recarga.",WS_CHILD|WS_VISIBLE,22,55,500,22,w,nullptr,nullptr,nullptr);g_status=CreateWindowW(L"STATIC",L"PRONTO",WS_CHILD|WS_VISIBLE|SS_CENTER,480,24,140,26,w,nullptr,nullptr,nullptr);g_button=CreateWindowW(L"BUTTON",L"DESBLOQUEAR DISCORD AGORA",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,20,88,600,54,w,(HMENU)100,nullptr,nullptr);g_progress=CreateWindowW(PROGRESS_CLASSW,nullptr,WS_CHILD|WS_VISIBLE|PBS_MARQUEE,20,154,600,8,w,nullptr,nullptr,nullptr);g_log=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"Abra o Discord e clique no botao acima.\r\nNenhuma janela de terminal sera aberta.\r\n",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,20,176,600,220,w,nullptr,nullptr,nullptr);SendMessage(g_button,WM_SETFONT,(WPARAM)font,TRUE);SendMessage(g_log,WM_SETFONT,(WPARAM)font,TRUE);return 0;}case WM_COMMAND:if(LOWORD(wp)==100){EnableWindow(g_button,FALSE);SetWindowTextW(g_status,L"TRABALHANDO");SendMessage(g_progress,PBM_SETMARQUEE,TRUE,25);CreateThread(nullptr,0,Worker,nullptr,0,nullptr);}return 0;case WM_LOG:{auto*s=(std::wstring*)lp;int n=GetWindowTextLengthW(g_log);SendMessage(g_log,EM_SETSEL,n,n);s->append(L"\r\n");SendMessage(g_log,EM_REPLACESEL,FALSE,(LPARAM)s->c_str());delete s;return 0;}case WM_DONE:SetWindowTextW(g_status,wp?L"CONCLUIDO":L"ERRO");SendMessage(g_progress,PBM_SETMARQUEE,FALSE,0);EnableWindow(g_button,TRUE);return 0;case WM_DESTROY:PostQuitMessage(0);return 0;}return DefWindowProc(w,m,wp,lp);}
int WINAPI wWinMain(HINSTANCE h,HINSTANCE,PWSTR,int show){INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_PROGRESS_CLASS};InitCommonControlsEx(&ic);WNDCLASSEXW c{sizeof(c)};c.lpfnWndProc=Proc;c.hInstance=h;c.hIcon=LoadIconW(h,MAKEINTRESOURCEW(1));c.hIconSm=c.hIcon;c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);c.lpszClassName=L"DiscordRefreshProxyWindow";RegisterClassExW(&c);g_main=CreateWindowExW(0,c.lpszClassName,L"Discord Refresh Proxy",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,660,450,nullptr,nullptr,h,nullptr);ShowWindow(g_main,show);UpdateWindow(g_main);MSG msg{};while(GetMessage(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessage(&msg);}return 0;}
