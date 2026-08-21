#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <commctrl.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <regex>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"wininet.lib")
#pragma comment(lib,"advapi32.lib")
namespace fs=std::filesystem;
constexpr UINT WM_LOG=WM_APP+1,WM_DONE=WM_APP+2;
constexpr wchar_t kInternetSettings[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
constexpr wchar_t kPacPath[]=L"/discord-refresh.pac"; // também serve de marca para limpar sobras de uma execução que morreu
constexpr wchar_t kBackupName[]=L"previous-autoconfig.txt";
constexpr int kHoldSeconds=45;                        // cobre o ciclo todo: recarga, reconexão do gateway e carga das guilds. Valor calibrado no uso real.
constexpr size_t kProbeBatch=8,kProbeLimit=24;
HWND g_main{},g_log{},g_button{},g_status{},g_progress{};

// wininet.h e winhttp.h não convivem no mesmo arquivo (INTERNET_* colidem), então a única função que falta vem declarada a mão.
extern "C" __declspec(dllimport) BOOL WINAPI InternetSetOptionW(LPVOID,DWORD,LPVOID,DWORD);
constexpr DWORD OPTION_REFRESH=37,OPTION_SETTINGS_CHANGED=39;

std::wstring Wide(const std::string&s){if(s.empty())return{};int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);std::wstring r(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n);return r;}
std::string Utf8(const std::wstring&s){if(s.empty())return{};int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);std::string r(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr);return r;}
void Log(const std::wstring&s){auto*copy=new std::wstring(s);if(!PostMessage(g_main,WM_LOG,0,(LPARAM)copy))delete copy;}
struct Internet{HINTERNET h{};explicit Internet(HINTERNET v=nullptr):h(v){}~Internet(){if(h)WinHttpCloseHandle(h);}operator HINTERNET()const{return h;}};

std::vector<BYTE> Get(const std::wstring&url,const std::wstring&proxy=L"",DWORD timeout=20000){
 URL_COMPONENTS p{sizeof(p)};wchar_t host[256]{},path[2048]{},extra[2048]{};p.lpszHostName=host;p.dwHostNameLength=256;p.lpszUrlPath=path;p.dwUrlPathLength=2048;p.lpszExtraInfo=extra;p.dwExtraInfoLength=2048;
 if(!WinHttpCrackUrl(url.c_str(),0,0,&p))throw std::runtime_error("URL inválida");
 Internet session(WinHttpOpen(L"DiscordRefreshProxy/3.0",proxy.empty()?WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY:WINHTTP_ACCESS_TYPE_NAMED_PROXY,proxy.empty()?WINHTTP_NO_PROXY_NAME:proxy.c_str(),WINHTTP_NO_PROXY_BYPASS,0));
 if(!session.h)throw std::runtime_error("WinHTTP indisponível");WinHttpSetTimeouts(session,timeout,timeout,timeout,timeout);
 Internet connection(WinHttpConnect(session,host,p.nPort,0));DWORD flags=p.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0;std::wstring requestPath=std::wstring(path)+extra;
 Internet request(WinHttpOpenRequest(connection,L"GET",requestPath.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,flags));
 if(!request.h||!WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request,nullptr))throw std::runtime_error("Falha na requisição HTTPS");
 DWORD status{},size=sizeof(status);WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
 if(status<200||status>=400)throw std::runtime_error("Servidor respondeu HTTP "+std::to_string(status));
 std::vector<BYTE> out;for(;;){DWORD available{};if(!WinHttpQueryDataAvailable(request,&available))throw std::runtime_error("Falha ao ler resposta");if(!available)break;size_t old=out.size();out.resize(old+available);DWORD read{};if(!WinHttpReadData(request,out.data()+old,available,&read))throw std::runtime_error("Falha no download");out.resize(old+read);}return out;
}
fs::path Root(){wchar_t b[MAX_PATH]{};if(!GetEnvironmentVariableW(L"LOCALAPPDATA",b,MAX_PATH))throw std::runtime_error("LOCALAPPDATA indisponível");return fs::path(b)/L"DiscordRefreshProxy";}

struct Proxy{std::wstring host;int port{};std::wstring country;};

// O Chromium remove path e query de URLs https antes de chamar FindProxyForURL, então só o host serve de critério.
std::string PacScript(const Proxy&p){
 return "function FindProxyForURL(url,host){"
        "if(host==\"discord.com\"||host==\"discordapp.com\"||host==\"discord.gg\"||"
        "dnsDomainIs(host,\".discord.com\")||dnsDomainIs(host,\".discordapp.com\")||"
        "dnsDomainIs(host,\".discordapp.net\")||dnsDomainIs(host,\".discord.gg\")||"
        "dnsDomainIs(host,\".discord.media\"))return \"PROXY "+Utf8(p.host)+":"+std::to_string(p.port)+"\";"
        "return \"DIRECT\";}";
}

// O Chromium deixou de aceitar PAC em file://, então o script vai por HTTP em loopback.
struct Pac{
 SOCKET listener{INVALID_SOCKET};HANDLE thread{},fetched{};std::string response;int port{};std::atomic<int> hits{0};
 ~Pac(){Stop();}
 // Esperar a leitura em vez de dormir um tempo fixo: o Ctrl+R só vale depois que o Discord adotou a configuração.
 bool WaitFetch(DWORD ms){return fetched&&WaitForSingleObject(fetched,ms)==WAIT_OBJECT_0;}
 void Start(const std::string&script){
  fetched=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(listener==INVALID_SOCKET)throw std::runtime_error("Socket local indisponível");
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=0;
  int length=sizeof(address);
  if(bind(listener,(sockaddr*)&address,length)||listen(listener,8)||getsockname(listener,(sockaddr*)&address,&length))throw std::runtime_error("Não foi possível abrir a porta local");
  port=ntohs(address.sin_port);
  response="HTTP/1.1 200 OK\r\nContent-Type: application/x-ns-proxy-autoconfig\r\nContent-Length: "+std::to_string(script.size())+"\r\nConnection: close\r\n\r\n"+script;
  thread=CreateThread(nullptr,0,Serve,this,0,nullptr);if(!thread)throw std::runtime_error("Não foi possível servir a configuração");
 }
 static DWORD WINAPI Serve(LPVOID argument){
  Pac*self=(Pac*)argument;
  for(;;){SOCKET client=accept(self->listener,nullptr,nullptr);if(client==INVALID_SOCKET)break;
   char discard[2048];recv(client,discard,sizeof(discard),0);
   send(client,self->response.data(),(int)self->response.size(),0);
   shutdown(client,SD_SEND);closesocket(client);
   ++self->hits;SetEvent(self->fetched);}
  return 0;
 }
 void Stop(){if(listener!=INVALID_SOCKET){closesocket(listener);listener=INVALID_SOCKET;}if(thread){WaitForSingleObject(thread,3000);CloseHandle(thread);thread=nullptr;}if(fetched){CloseHandle(fetched);fetched=nullptr;}}
};

std::wstring AutoConfig(){wchar_t b[2048]{};DWORD n=sizeof(b);return RegGetValueW(HKEY_CURRENT_USER,kInternetSettings,L"AutoConfigURL",RRF_RT_REG_SZ,nullptr,b,&n)==ERROR_SUCCESS?std::wstring(b):std::wstring{};}
void SetAutoConfig(const std::wstring&url){
 HKEY key{};if(RegOpenKeyExW(HKEY_CURRENT_USER,kInternetSettings,0,KEY_SET_VALUE,&key)!=ERROR_SUCCESS)throw std::runtime_error("Sem acesso às configurações de proxy do usuário");
 LSTATUS status=url.empty()?RegDeleteValueW(key,L"AutoConfigURL"):RegSetValueExW(key,L"AutoConfigURL",0,REG_SZ,(const BYTE*)url.c_str(),(DWORD)((url.size()+1)*sizeof(wchar_t)));
 RegCloseKey(key);if(!url.empty()&&status!=ERROR_SUCCESS)throw std::runtime_error("Não foi possível aplicar o proxy");
 InternetSetOptionW(nullptr,OPTION_SETTINGS_CHANGED,nullptr,0);InternetSetOptionW(nullptr,OPTION_REFRESH,nullptr,0);
}
// Restaura o valor anterior mesmo se algo lançar no meio: um AutoConfigURL órfão deixaria o Discord apontando para um servidor morto.
// O valor anterior também vai para disco porque quem tinha um PAC corporativo o perderia se o processo morresse antes de restaurar.
struct ProxyScope{
 std::wstring previous;bool active{};
 explicit ProxyScope(const std::wstring&url):previous(AutoConfig()){
  try{fs::create_directories(Root());std::ofstream(Root()/kBackupName,std::ios::binary)<<Utf8(previous);}catch(...){}
  SetAutoConfig(url);active=true;}
 ~ProxyScope(){Release();}
 void Release(){if(!active)return;active=false;try{SetAutoConfig(previous);std::error_code ec;fs::remove(Root()/kBackupName,ec);}catch(...){}}
};
void ClearStaleProxy(){try{
 if(AutoConfig().find(kPacPath)==std::wstring::npos)return;
 std::string saved;{std::ifstream f(Root()/kBackupName,std::ios::binary);std::getline(f,saved);}
 SetAutoConfig(Wide(saved));std::error_code ec;fs::remove(Root()/kBackupName,ec);}catch(...){}}

std::vector<Proxy> ParseProxies(const std::string&body,const std::wstring&country){
 std::vector<Proxy> out;const std::regex re(R"((\d{1,3}(?:\.\d{1,3}){3}):(\d{2,5}))");
 for(std::sregex_iterator i(body.begin(),body.end(),re),e;i!=e;++i)out.push_back({Wide((*i)[1]),std::stoi((*i)[2]),country});
 return out;
}
std::vector<Proxy> Fetch(std::wstring code,std::wstring country){
 try{auto b=Get(L"https://api.proxyscrape.com/v4/free-proxy-list/get?request=getproxies&protocol=http&country="+code+L"&timeout=5000&ssl=yes&anonymity=elite,anonymous&limit=80");
  return ParseProxies(std::string(b.begin(),b.end()),country);}catch(...){return {};}
}
bool Probe(std::wstring host,int port){try{Get(L"https://discord.com/api/v10/gateway",host+L":"+std::to_wstring(port),7000);return true;}catch(...){return false;}}
Proxy LoadLast(){try{std::ifstream f(Root()/L"last-proxy.txt");std::string line;if(!std::getline(f,line))return{};size_t colon=line.rfind(':');if(colon==std::string::npos)return{};return{Wide(line.substr(0,colon)),std::stoi(line.substr(colon+1)),L"último aprovado"};}catch(...){return{};}}
void SaveLast(const Proxy&p){try{fs::create_directories(Root());std::ofstream(Root()/L"last-proxy.txt")<<Utf8(p.host)<<":"<<p.port;}catch(...){}}

Proxy FindProxy(){
 std::vector<std::pair<std::wstring,std::wstring>> countries={{L"US",L"Estados Unidos"},{L"CA",L"Canadá"},{L"NL",L"Países Baixos"},{L"DE",L"Alemanha"},{L"FR",L"França"},{L"GB",L"Reino Unido"},{L"JP",L"Japão"},{L"SG",L"Singapura"}};
 Log(L"Buscando listas de proxies em "+std::to_wstring(countries.size())+L" países...");
 std::vector<std::future<std::vector<Proxy>>> lists;for(auto&c:countries)lists.push_back(std::async(std::launch::async,Fetch,c.first,c.second));
 std::vector<Proxy> pool;for(auto&l:lists){auto found=l.get();pool.insert(pool.end(),found.begin(),found.end());}
 if(pool.empty())throw std::runtime_error("Nenhuma lista pública de proxies respondeu");
 std::mt19937 rng((unsigned)GetTickCount64());std::shuffle(pool.begin(),pool.end(),rng);
 if(Proxy last=LoadLast();!last.host.empty())pool.insert(pool.begin(),last);
 Log(L"Testando até "+std::to_wstring(kProbeLimit)+L" proxies contra o Discord...");
 for(size_t start=0,limit=(std::min)(pool.size(),kProbeLimit);start<limit;start+=kProbeBatch){
  size_t end=(std::min)(start+kProbeBatch,limit);
  std::vector<std::future<bool>> probes;for(size_t i=start;i<end;++i)probes.push_back(std::async(std::launch::async,Probe,pool[i].host,pool[i].port));
  std::vector<char> alive(probes.size());for(size_t i=0;i<probes.size();++i)alive[i]=probes[i].get()?1:0;
  for(size_t i=0;i<alive.size();++i)if(alive[i]){SaveLast(pool[start+i]);return pool[start+i];}
  Log(L"  Nenhum respondeu nesta rodada, tentando a próxima...");
 }
 throw std::runtime_error("Nenhum proxy público funcional encontrado. Tente novamente.");
}

BOOL CALLBACK DiscordEnum(HWND w,LPARAM out){if(!IsWindowVisible(w))return TRUE;DWORD pid{};GetWindowThreadProcessId(w,&pid);HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!h)return TRUE;wchar_t path[32768]{};DWORD n=32768;QueryFullProcessImageNameW(h,0,path,&n);CloseHandle(h);std::wstring name=fs::path(path).filename().wstring();std::transform(name.begin(),name.end(),name.begin(),::towlower);if(name==L"discord.exe"||name==L"discordcanary.exe"||name==L"discordptb.exe"||name==L"discorddevelopment.exe"){*(HWND*)out=w;return FALSE;}return TRUE;}
HWND Discord(){HWND w{};EnumWindows(DiscordEnum,(LPARAM)&w);return w;}
void Refresh(HWND w){ShowWindowAsync(w,SW_RESTORE);SetWindowPos(w,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);SetWindowPos(w,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);DWORD target=GetWindowThreadProcessId(w,nullptr),current=GetCurrentThreadId();BOOL attached=target!=current&&AttachThreadInput(current,target,TRUE);BringWindowToTop(w);SetForegroundWindow(w);SetFocus(w);Sleep(700);if(attached)AttachThreadInput(current,target,FALSE);if(GetForegroundWindow()==w){keybd_event(VK_CONTROL,0,0,0);keybd_event('R',0,0,0);Sleep(80);keybd_event('R',0,KEYEVENTF_KEYUP,0);keybd_event(VK_CONTROL,0,KEYEVENTF_KEYUP,0);}else{PostMessage(w,WM_KEYDOWN,VK_CONTROL,0);PostMessage(w,WM_KEYDOWN,'R',0);Sleep(80);PostMessage(w,WM_KEYUP,'R',0);PostMessage(w,WM_KEYUP,VK_CONTROL,0);}}

DWORD WINAPI Worker(LPVOID){
 try{
  HWND discord=Discord();if(!discord)throw std::runtime_error("Abra o Discord antes de continuar");Log(L"Discord encontrado.");
  Proxy proxy=FindProxy();Log(L"Proxy aprovado: "+proxy.host+L":"+std::to_wstring(proxy.port)+L" - "+proxy.country);
  Pac pac;pac.Start(PacScript(proxy));
  ProxyScope scope(L"http://127.0.0.1:"+std::to_wstring(pac.port)+kPacPath);
  Log(L"Proxy aplicado somente aos domínios do Discord.");
  Log(pac.WaitFetch(4000)?L"Discord leu a configuração.":L"Sem confirmação de leitura, seguindo assim mesmo.");
  Refresh(discord);Log(L"Discord recarregado. Mantendo o proxy por "+std::to_wstring(kHoldSeconds)+L"s até ele voltar por completo...");
  // Um proxy público pode morrer no meio da recarga; sem esse teste o usuário veria "concluído" com o Discord pela metade.
  for(int remaining=kHoldSeconds;remaining>0;--remaining){
   SetWindowTextW(g_status,(L"AGUARDE "+std::to_wstring(remaining)+L"s").c_str());Sleep(1000);
   if(remaining>1&&remaining%15==0&&!Probe(proxy.host,proxy.port))throw std::runtime_error("O proxy caiu antes de a recarga terminar");
  }
  scope.Release();Log(L"Configuração lida "+std::to_wstring(pac.hits.load())+L" vez(es). Proxy removido.");
  Log(L"Concluído usando saída em "+proxy.country+L".");
  PostMessage(g_main,WM_DONE,TRUE,0);
 }catch(const std::exception&e){Log(L"ERRO: "+Wide(e.what()));PostMessage(g_main,WM_DONE,FALSE,0);}
 return 0;
}

int SelfTest(){
 Proxy sample{L"203.0.113.7",8080,L"teste"};std::string script=PacScript(sample);
 if(script.find("PROXY 203.0.113.7:8080")==std::string::npos)return 1;
 if(script.find("return \"DIRECT\"")==std::string::npos)return 1;
 if(script.find("dnsDomainIs(host,\".discord.gg\")")==std::string::npos)return 1;
 auto parsed=ParseProxies("1.2.3.4:8080\r\nlixo sem porta\r\n10.0.0.1:3128\n",L"teste");
 if(parsed.size()!=2||parsed[0].port!=8080||parsed[0].host!=L"1.2.3.4"||parsed[1].host!=L"10.0.0.1"||parsed[1].port!=3128)return 1;
 if(!ParseProxies("nada aqui",L"teste").empty())return 1;
 return 0;
}

LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM wp,LPARAM lp){switch(m){case WM_CREATE:{HFONT font=CreateFontW(18,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");CreateWindowW(L"STATIC",L"Discord Refresh Proxy",WS_CHILD|WS_VISIBLE,20,18,400,34,w,nullptr,nullptr,nullptr);CreateWindowW(L"STATIC",L"Proxy temporário apenas nos domínios do Discord.",WS_CHILD|WS_VISIBLE,22,55,500,22,w,nullptr,nullptr,nullptr);g_status=CreateWindowW(L"STATIC",L"PRONTO",WS_CHILD|WS_VISIBLE|SS_CENTER,480,24,140,26,w,nullptr,nullptr,nullptr);g_button=CreateWindowW(L"BUTTON",L"DESBLOQUEAR DISCORD AGORA",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,20,88,600,54,w,(HMENU)100,nullptr,nullptr);g_progress=CreateWindowW(PROGRESS_CLASSW,nullptr,WS_CHILD|WS_VISIBLE|PBS_MARQUEE,20,154,600,8,w,nullptr,nullptr,nullptr);g_log=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"Abra o Discord e clique no botão acima.\r\nNenhuma janela de terminal será aberta.\r\n",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,20,176,600,220,w,nullptr,nullptr,nullptr);SendMessage(g_button,WM_SETFONT,(WPARAM)font,TRUE);SendMessage(g_log,WM_SETFONT,(WPARAM)font,TRUE);return 0;}case WM_COMMAND:if(LOWORD(wp)==100){EnableWindow(g_button,FALSE);SetWindowTextW(g_status,L"TRABALHANDO");SendMessage(g_progress,PBM_SETMARQUEE,TRUE,25);CloseHandle(CreateThread(nullptr,0,Worker,nullptr,0,nullptr));}return 0;case WM_LOG:{auto*s=(std::wstring*)lp;int n=GetWindowTextLengthW(g_log);SendMessage(g_log,EM_SETSEL,n,n);s->append(L"\r\n");SendMessage(g_log,EM_REPLACESEL,FALSE,(LPARAM)s->c_str());delete s;return 0;}case WM_DONE:SetWindowTextW(g_status,wp?L"CONCLUÍDO":L"ERRO");SendMessage(g_progress,PBM_SETMARQUEE,FALSE,0);EnableWindow(g_button,TRUE);return 0;case WM_DESTROY:PostQuitMessage(0);return 0;}return DefWindowProc(w,m,wp,lp);}
int WINAPI wWinMain(HINSTANCE h,HINSTANCE,PWSTR,int show){
 if(wcsstr(GetCommandLineW(),L"--selftest"))return SelfTest();
 WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa))return 1;
 ClearStaleProxy();
 INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_PROGRESS_CLASS};InitCommonControlsEx(&ic);WNDCLASSEXW c{sizeof(c)};c.lpfnWndProc=Proc;c.hInstance=h;c.hIcon=LoadIconW(h,MAKEINTRESOURCEW(1));c.hIconSm=c.hIcon;c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);c.lpszClassName=L"DiscordRefreshProxyWindow";RegisterClassExW(&c);g_main=CreateWindowExW(0,c.lpszClassName,L"Discord Refresh Proxy",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,660,450,nullptr,nullptr,h,nullptr);ShowWindow(g_main,show);UpdateWindow(g_main);MSG msg{};while(GetMessage(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessage(&msg);}return 0;}
