#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <commctrl.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <regex>
#include <random>
#include <set>
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
constexpr wchar_t kBlockedName[]=L"blocked.txt";
constexpr int kVerifySeconds=40;                      // tempo dado ao Discord para voltar antes de julgar o proxy
constexpr int kGraceSeconds=10;                       // folga depois da recarga confirmada, para o que o Discord ainda pede logo após montar
constexpr size_t kProbeBatch=12,kProbeLimit=60,kMaxAttempts=3;
// O bundle do /app tem ~3,2 MB comprimidos. A 200 KB/s ele chega em ~16 s e a recarga cabe na janela de
// verificação; a 33 KB/s, que é o que um proxy público típico entrega, levaria mais de um minuto e meio: tela cinza.
// Barra medida contra a fonte real: numa amostra de 25 proxies públicos, 5 passavam de 200 KB/s e 2 de 300.
// Daí a amostragem larga — a maior parte da lista é lenta demais e precisa ser descartada em bloco.
constexpr size_t kSpeedSample=512*1024;
constexpr DWORD kSpeedBudgetMs=4000;                  // amostra time-boxed: quem não entregou o pedaço em 4 s já se reprovou
constexpr int kMinKBps=200;
constexpr long long kBlockDays=7;                     // proxy público troca de dono e de rota em poucos dias; veto eterno só encolhe a lista útil
HWND g_main{},g_log{},g_button{},g_status{},g_progress{};

// wininet.h e winhttp.h não convivem no mesmo arquivo (INTERNET_* colidem), então a única função que falta vem declarada a mão.
extern "C" __declspec(dllimport) BOOL WINAPI InternetSetOptionW(LPVOID,DWORD,LPVOID,DWORD);
constexpr DWORD OPTION_REFRESH=37,OPTION_SETTINGS_CHANGED=39;

std::wstring Wide(const std::string&s){if(s.empty())return{};int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);std::wstring r(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n);return r;}
std::string Utf8(const std::wstring&s){if(s.empty())return{};int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);std::string r(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr);return r;}
void Log(const std::wstring&s){auto*copy=new std::wstring(s);if(!PostMessage(g_main,WM_LOG,0,(LPARAM)copy))delete copy;}
struct Internet{HINTERNET h{};explicit Internet(HINTERNET v=nullptr):h(v){}~Internet(){if(h)WinHttpCloseHandle(h);}operator HINTERNET()const{return h;}};

// cap corta a leitura assim que houver bytes suficientes (medir vazão não exige baixar 3 MB);
// anyStatus aceita qualquer resposta HTTP, para os testes em que o que importa é o túnel ter subido.
// budget é teto de tempo do corpo: o timeout do WinHTTP é por leitura, então um proxy que goteja
// bytes nunca o dispara e prenderia a rodada inteira. Com o teto, ele volta com o pouco que entregou.
// readMs devolve só a duração do corpo, sem o handshake, para não cobrar latência como se fosse vazão.
std::vector<BYTE> Get(const std::wstring&url,const std::wstring&proxy=L"",DWORD timeout=20000,size_t cap=(size_t)-1,bool anyStatus=false,const std::wstring&headers=L"",DWORD budget=0,ULONGLONG*readMs=nullptr){
 URL_COMPONENTS p{sizeof(p)};wchar_t host[256]{},path[2048]{},extra[2048]{};p.lpszHostName=host;p.dwHostNameLength=256;p.lpszUrlPath=path;p.dwUrlPathLength=2048;p.lpszExtraInfo=extra;p.dwExtraInfoLength=2048;
 if(!WinHttpCrackUrl(url.c_str(),0,0,&p))throw std::runtime_error("URL inválida");
 Internet session(WinHttpOpen(L"DiscordRefreshProxy/3.0",proxy.empty()?WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY:WINHTTP_ACCESS_TYPE_NAMED_PROXY,proxy.empty()?WINHTTP_NO_PROXY_NAME:proxy.c_str(),WINHTTP_NO_PROXY_BYPASS,0));
 if(!session.h)throw std::runtime_error("WinHTTP indisponível");WinHttpSetTimeouts(session,timeout,timeout,timeout,timeout);
 Internet connection(WinHttpConnect(session,host,p.nPort,0));DWORD flags=p.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0;std::wstring requestPath=std::wstring(path)+extra;
 Internet request(WinHttpOpenRequest(connection,L"GET",requestPath.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,flags));
 if(!request.h||!WinHttpSendRequest(request,headers.empty()?(LPCWSTR)nullptr:headers.c_str(),headers.empty()?(DWORD)0:(DWORD)-1L,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request,nullptr))throw std::runtime_error("Falha na requisição HTTPS");
 DWORD status{},size=sizeof(status);WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
 if(!anyStatus&&(status<200||status>=400))throw std::runtime_error("Servidor respondeu HTTP "+std::to_string(status));
 ULONGLONG body=GetTickCount64();std::vector<BYTE> out;
 for(;;){DWORD available{};if(!WinHttpQueryDataAvailable(request,&available))throw std::runtime_error("Falha ao ler resposta");if(!available)break;size_t old=out.size();out.resize(old+available);DWORD read{};if(!WinHttpReadData(request,out.data()+old,available,&read))throw std::runtime_error("Falha no download");out.resize(old+read);
  if(out.size()>=cap)break;if(budget&&GetTickCount64()-body>=budget)break;}
 if(readMs)*readMs=(std::max)(GetTickCount64()-body,(ULONGLONG)1);
 return out;
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
// O HTML do /app e so a casca cinza: quem desenha a tela e o bundle que ele manda baixar.
bool IsAppShell(const std::string&html){return html.find("GLOBAL_ENV")!=std::string::npos&&html.find("/assets/")!=std::string::npos;}
std::string FirstAsset(const std::string&html){std::smatch m;const std::regex re(R"(/assets/[A-Za-z0-9._-]+\.js)");return std::regex_search(html,m,re)?m.str():std::string{};}
// Alcance nao era o criterio certo. Medido no proxy que a versao anterior aprovou: HTML em 1 s, CDN e
// WebSocket de pe, e o bundle a 33 KB/s - mais de um minuto e meio para 3,2 MB, com a janela cinza o tempo todo.
// Entao o veredito e vazao, medida num pedaco do bundle de verdade. Devolve KB/s, ou 0 se reprovado.
int Probe(std::wstring host,int port){
 std::wstring via=host+L":"+std::to_wstring(port);
 try{
  Get(L"https://gateway.discord.gg/?v=10&encoding=json",via,4000,4096,true); // sem upgrade a resposta e 4xx; o que importa e o tunel do WebSocket ter subido
  auto raw=Get(L"https://discord.com/app",via,6000,262144);
  std::string html(raw.begin(),raw.end());std::string asset=FirstAsset(html);
  if(!IsAppShell(html)||asset.empty())return 0;                              // bloqueio de ISP e portal de captura tambem devolvem 200 com HTML proprio
  ULONGLONG ms=1;
  size_t got=Get(L"https://discord.com"+Wide(asset),via,8000,kSpeedSample,false,L"Range: bytes=0-"+std::to_wstring(kSpeedSample-1),kSpeedBudgetMs,&ms).size();
  int kbps=(int)(got*1000/ms/1024);
  return kbps>=kMinKBps?kbps:0;
 }catch(...){return 0;}
}
std::wstring Title(HWND w){wchar_t b[512]{};GetWindowTextW(w,b,512);return b;}
// Veredito conservador: so acusa tela cinza se o titulo saiu do que era e nao voltou.
// Falso negativo custa uma tentativa a mais; falso positivo descartaria um proxy bom para sempre.
struct ReloadWatch{
 std::wstring good;bool changed{};
 explicit ReloadWatch(std::wstring t):good(std::move(t)){}
 bool Reloaded(const std::wstring&now){if(now!=good){changed=true;return false;}return changed;}
 bool AcceptOnTimeout()const{return !changed;}
};
bool WaitReload(HWND w,const std::wstring&good,int seconds){
 ReloadWatch watch(good);
 for(int i=0;i<seconds*2;++i){Sleep(500);if(watch.Reloaded(Title(w)))return true;}
 return watch.AcceptOnTimeout();
}
std::string Key(const Proxy&p){return Utf8(p.host)+":"+std::to_string(p.port);}
long long Today(){return std::chrono::duration_cast<std::chrono::hours>(std::chrono::system_clock::now().time_since_epoch()).count()/24;}
// Linha sem data veio do veto antigo, que julgava só pelo título da janela e condenava proxy bom para sempre.
std::string LiveBlocked(const std::string&line,long long today){
 size_t bar=line.rfind('|');if(bar==std::string::npos||!bar)return {};
 try{if(today-std::stoll(line.substr(bar+1))<kBlockDays)return line.substr(0,bar);}catch(...){}
 return {};
}
std::set<std::string> LoadBlocked(){
 std::set<std::string> out;std::vector<std::string> keep;size_t total=0;long long today=Today();
 try{
  {std::ifstream f(Root()/kBlockedName);for(std::string line;std::getline(f,line);){if(line.empty())continue;++total;if(std::string key=LiveBlocked(line,today);!key.empty()){out.insert(key);keep.push_back(line);}}}
  if(keep.size()!=total){std::ofstream f(Root()/kBlockedName,std::ios::trunc);for(const auto&line:keep)f<<line<<"\n";}
 }catch(...){}
 return out;
}
void Block(const Proxy&p){try{fs::create_directories(Root());std::ofstream(Root()/kBlockedName,std::ios::app)<<Key(p)<<"|"<<Today()<<"\n";}catch(...){}}
Proxy LoadLast(){try{std::ifstream f(Root()/L"last-proxy.txt");std::string line;if(!std::getline(f,line))return{};size_t colon=line.rfind(':');if(colon==std::string::npos)return{};return{Wide(line.substr(0,colon)),std::stoi(line.substr(colon+1)),L"último aprovado"};}catch(...){return{};}}
void SaveLast(const Proxy&p){try{fs::create_directories(Root());std::ofstream(Root()/L"last-proxy.txt")<<Utf8(p.host)<<":"<<p.port;}catch(...){}}

// Devolve os aprovados ordenados do mais rapido para o mais lento, com a vazao medida junto:
// se o primeiro nao recarregar o Discord, ha suplentes prontos e ja classificados.
std::vector<std::pair<int,Proxy>> FindProxies(){
 std::vector<std::pair<std::wstring,std::wstring>> countries={{L"US",L"Estados Unidos"},{L"MX",L"México"},{L"DE",L"Alemanha"},{L"ES",L"Espanha"},{L"IT",L"Itália"},{L"PT",L"Portugal"},{L"AR",L"Argentina"},{L"UY",L"Uruguai"},{L"PY",L"Paraguai"}};
 Log(L"Buscando listas de proxies em "+std::to_wstring(countries.size())+L" países...");
 std::vector<std::future<std::vector<Proxy>>> lists;for(auto&c:countries)lists.push_back(std::async(std::launch::async,Fetch,c.first,c.second));
 std::vector<Proxy> pool;for(auto&l:lists){auto found=l.get();pool.insert(pool.end(),found.begin(),found.end());}
 if(pool.empty())throw std::runtime_error("Nenhuma lista pública de proxies respondeu");
 std::set<std::string> blocked=LoadBlocked();size_t before=pool.size();
 pool.erase(std::remove_if(pool.begin(),pool.end(),[&](const Proxy&p){return blocked.count(Key(p))>0;}),pool.end());
 if(size_t cut=before-pool.size();cut)Log(L"Ignorando "+std::to_wstring(cut)+L" proxies que já falharam antes.");
 if(pool.empty())throw std::runtime_error("Todos os proxies encontrados já falharam antes. Tente novamente mais tarde.");
 std::mt19937 rng((unsigned)GetTickCount64());std::shuffle(pool.begin(),pool.end(),rng);
 if(Proxy last=LoadLast();!last.host.empty()&&!blocked.count(Key(last)))pool.insert(pool.begin(),last);
 Log(L"Testando até "+std::to_wstring(kProbeLimit)+L" proxies: mínimo de "+std::to_wstring(kMinKBps)+L" KB/s para dar conta do bundle do Discord.");
 std::vector<std::pair<int,Proxy>> ranked;
 for(size_t start=0,limit=(std::min)(pool.size(),kProbeLimit);start<limit&&ranked.size()<kMaxAttempts;start+=kProbeBatch){
  size_t end=(std::min)(start+kProbeBatch,limit);
  std::vector<std::future<int>> probes;for(size_t i=start;i<end;++i)probes.push_back(std::async(std::launch::async,Probe,pool[i].host,pool[i].port));
  std::vector<int> speed(probes.size());for(size_t i=0;i<probes.size();++i)speed[i]=probes[i].get();
  for(size_t i=0;i<speed.size();++i)if(speed[i])ranked.push_back({speed[i],pool[start+i]});
  Log(L"  Rodada "+std::to_wstring(start/kProbeBatch+1)+L": "+std::to_wstring(ranked.size())+L" aprovado(s) até aqui.");
 }
 if(ranked.empty())throw std::runtime_error("Nenhum proxy público com velocidade suficiente. Os que responderam entregariam o Discord lento demais e a tela ficaria cinza. Tente de novo em alguns minutos.");
 std::sort(ranked.begin(),ranked.end(),[](const auto&a,const auto&b){return a.first>b.first;});
 return ranked;
}

BOOL CALLBACK DiscordEnum(HWND w,LPARAM out){if(!IsWindowVisible(w))return TRUE;DWORD pid{};GetWindowThreadProcessId(w,&pid);HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!h)return TRUE;wchar_t path[32768]{};DWORD n=32768;QueryFullProcessImageNameW(h,0,path,&n);CloseHandle(h);std::wstring name=fs::path(path).filename().wstring();std::transform(name.begin(),name.end(),name.begin(),::towlower);if(name==L"discord.exe"||name==L"discordcanary.exe"||name==L"discordptb.exe"||name==L"discorddevelopment.exe"){*(HWND*)out=w;return FALSE;}return TRUE;}
HWND Discord(){HWND w{};EnumWindows(DiscordEnum,(LPARAM)&w);return w;}
void Refresh(HWND w){ShowWindowAsync(w,SW_RESTORE);SetWindowPos(w,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);SetWindowPos(w,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);DWORD target=GetWindowThreadProcessId(w,nullptr),current=GetCurrentThreadId();BOOL attached=target!=current&&AttachThreadInput(current,target,TRUE);BringWindowToTop(w);SetForegroundWindow(w);SetFocus(w);Sleep(700);if(attached)AttachThreadInput(current,target,FALSE);if(GetForegroundWindow()==w){keybd_event(VK_CONTROL,0,0,0);keybd_event('R',0,0,0);Sleep(80);keybd_event('R',0,KEYEVENTF_KEYUP,0);keybd_event(VK_CONTROL,0,KEYEVENTF_KEYUP,0);}else{PostMessage(w,WM_KEYDOWN,VK_CONTROL,0);PostMessage(w,WM_KEYDOWN,'R',0);Sleep(80);PostMessage(w,WM_KEYUP,'R',0);PostMessage(w,WM_KEYUP,VK_CONTROL,0);}}

DWORD WINAPI Worker(LPVOID){
 try{
  HWND discord=Discord();if(!discord)throw std::runtime_error("Abra o Discord antes de continuar");Log(L"Discord encontrado.");
  std::wstring good=Title(discord); // título com o Discord carregado: é a ele que a janela precisa voltar depois do Ctrl+R
  if(good.empty()||good==L"Discord")Log(L"Aviso: o título da janela não mostra um canal, então a recarga não pode ser julgada por ele. Abra um canal antes de clicar para o descarte automático funcionar.");
  std::vector<std::pair<int,Proxy>> candidates=FindProxies();
  Log(std::to_wstring(candidates.size())+L" proxy(s) passaram no teste de vazão.");
  for(size_t attempt=0;attempt<candidates.size()&&attempt<kMaxAttempts;++attempt){
   const Proxy&proxy=candidates[attempt].second;
   Log(L"Tentativa "+std::to_wstring(attempt+1)+L": "+proxy.host+L":"+std::to_wstring(proxy.port)+L" - "+proxy.country+L" - "+std::to_wstring(candidates[attempt].first)+L" KB/s");
   Pac pac;pac.Start(PacScript(proxy));
   ProxyScope scope(L"http://127.0.0.1:"+std::to_wstring(pac.port)+kPacPath);
   Log(pac.WaitFetch(4000)?L"Discord leu a configuração.":L"Sem confirmação de leitura, seguindo assim mesmo.");
   Refresh(discord);SetWindowTextW(g_status,L"RECARREGANDO");
   if(!WaitReload(discord,good,kVerifySeconds)){
    Block(proxy);Log(L"O Discord não voltou da recarga (tela cinza). Proxy descartado de vez.");
    continue; // o PAC deste proxy sai de cena aqui, antes da próxima tentativa
   }
   // O título só volta quando a interface montou, então a essa altura o bundle já desceu inteiro e o
   // que resta são requisições curtas. A folga cobre essas, e é contada da confirmação, não do Ctrl+R:
   // esperar um prazo fixo desde a recarga cobrava do usuário um tempo que a evidência já dispensava.
   Log(L"Recarga confirmada. Segurando o proxy por mais "+std::to_wstring(kGraceSeconds)+L"s.");
   for(int remaining=kGraceSeconds;remaining>0;--remaining){SetWindowTextW(g_status,(L"AGUARDE "+std::to_wstring(remaining)+L"s").c_str());Sleep(1000);}
   SaveLast(proxy);scope.Release();
   Log(L"Configuração lida "+std::to_wstring(pac.hits.load())+L" vez(es). Proxy removido.");
   Log(L"Concluído usando saída em "+proxy.country+L".");
   PostMessage(g_main,WM_DONE,TRUE,0);return 0;
  }
  throw std::runtime_error("Nenhum proxy conseguiu recarregar o Discord. Clique de novo para tentar com outros.");
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
 ReloadWatch voltou(L"#geral | Servidor");
 if(voltou.Reloaded(L"#geral | Servidor"))return 1;   // ainda não mudou: nada a declarar
 if(voltou.Reloaded(L"Discord"))return 1;             // entrou em carga
 if(!voltou.Reloaded(L"#geral | Servidor"))return 1;  // voltou ao que era: recarregou
 ReloadWatch cinza(L"#geral | Servidor");
 cinza.Reloaded(L"Discord");
 if(cinza.AcceptOnTimeout())return 1;                 // mudou e não voltou: tela cinza, descarta o proxy
 ReloadWatch mudo(L"Discord");
 mudo.Reloaded(L"Discord");
 if(!mudo.AcceptOnTimeout())return 1;                 // título nunca mudou: inconclusivo, não culpa o proxy
 const std::string shell="<script>window.GLOBAL_ENV={};</script><link href=\"/assets/a.css\"><script src=\"/assets/web.e7ec05b4.js\"></script>";
 if(!IsAppShell(shell))return 1;
 if(IsAppShell("<html><head><title>Acesso bloqueado</title></head><body>Politica de uso</body></html>"))return 1;
 if(FirstAsset(shell)!="/assets/web.e7ec05b4.js")return 1;  // o .css vem antes no HTML e nao pode ser escolhido
 if(!FirstAsset("<html>sem bundle</html>").empty())return 1;
 if(LiveBlocked("1.2.3.4:8080|100",100)!="1.2.3.4:8080")return 1;
 if(!LiveBlocked("1.2.3.4:8080|100",100+kBlockDays).empty())return 1;  // veto vence e o proxy volta ao sorteio
 if(!LiveBlocked("1.2.3.4:8080",100).empty())return 1;                 // formato antigo, sem data
 return 0;
}

LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM wp,LPARAM lp){switch(m){case WM_CREATE:{HFONT font=CreateFontW(18,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");CreateWindowW(L"STATIC",L"Discord Refresh Proxy",WS_CHILD|WS_VISIBLE,20,18,400,34,w,nullptr,nullptr,nullptr);CreateWindowW(L"STATIC",L"Proxy temporário apenas nos domínios do Discord.",WS_CHILD|WS_VISIBLE,22,55,500,22,w,nullptr,nullptr,nullptr);g_status=CreateWindowW(L"STATIC",L"PRONTO",WS_CHILD|WS_VISIBLE|SS_CENTER,480,24,140,26,w,nullptr,nullptr,nullptr);g_button=CreateWindowW(L"BUTTON",L"DESBLOQUEAR DISCORD AGORA",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,20,88,600,54,w,(HMENU)100,nullptr,nullptr);g_progress=CreateWindowW(PROGRESS_CLASSW,nullptr,WS_CHILD|WS_VISIBLE|PBS_MARQUEE,20,154,600,8,w,nullptr,nullptr,nullptr);g_log=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"Abra o Discord e clique no botão acima.\r\nNenhuma janela de terminal será aberta.\r\n",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,20,176,600,220,w,nullptr,nullptr,nullptr);SendMessage(g_button,WM_SETFONT,(WPARAM)font,TRUE);SendMessage(g_log,WM_SETFONT,(WPARAM)font,TRUE);return 0;}case WM_COMMAND:if(LOWORD(wp)==100){EnableWindow(g_button,FALSE);SetWindowTextW(g_status,L"TRABALHANDO");SendMessage(g_progress,PBM_SETMARQUEE,TRUE,25);CloseHandle(CreateThread(nullptr,0,Worker,nullptr,0,nullptr));}return 0;case WM_LOG:{auto*s=(std::wstring*)lp;int n=GetWindowTextLengthW(g_log);SendMessage(g_log,EM_SETSEL,n,n);s->append(L"\r\n");SendMessage(g_log,EM_REPLACESEL,FALSE,(LPARAM)s->c_str());delete s;return 0;}case WM_DONE:SetWindowTextW(g_status,wp?L"CONCLUÍDO":L"ERRO");SendMessage(g_progress,PBM_SETMARQUEE,FALSE,0);EnableWindow(g_button,TRUE);return 0;case WM_DESTROY:PostQuitMessage(0);return 0;}return DefWindowProc(w,m,wp,lp);}
int WINAPI wWinMain(HINSTANCE h,HINSTANCE,PWSTR,int show){
 if(wcsstr(GetCommandLineW(),L"--selftest"))return SelfTest();
 WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa))return 1;
 ClearStaleProxy();
 INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_PROGRESS_CLASS};InitCommonControlsEx(&ic);WNDCLASSEXW c{sizeof(c)};c.lpfnWndProc=Proc;c.hInstance=h;c.hIcon=LoadIconW(h,MAKEINTRESOURCEW(1));c.hIconSm=c.hIcon;c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);c.lpszClassName=L"DiscordRefreshProxyWindow";RegisterClassExW(&c);g_main=CreateWindowExW(0,c.lpszClassName,L"Discord Refresh Proxy",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,660,450,nullptr,nullptr,h,nullptr);ShowWindow(g_main,show);UpdateWindow(g_main);MSG msg{};while(GetMessage(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessage(&msg);}return 0;}
