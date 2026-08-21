# Discord Refresh Proxy

Aplicativo nativo C++/Win32 de um clique. Não requer .NET, PowerShell, instalação, Administrador ou download adicional, e não abre console.

Ele encontra um proxy público estrangeiro com HTTPS, restrito a uma lista fixa de países (Estados Unidos, México, Alemanha, Espanha, Itália, Portugal, Argentina, Uruguai e Paraguai), aplica esse proxy **somente aos domínios do Discord** por meio de um arquivo PAC temporário, envia `Ctrl+R`, espera a recarga terminar e remove a configuração 10 segundos depois. O gatilho da saída é a evidência, não o relógio: assim que o título da janela mostra que a interface voltou, o proxy só fica mais essa folga curta, para cobrir as requisições que o Discord ainda faz logo após montar. Todo o resto da navegação continua direto o tempo todo.

O critério de aceitação é vazão, não alcance. O HTML de `discord.com/app` é só a casca cinza; quem desenha a tela é o bundle de JavaScript que essa casca manda baixar, com cerca de 3,2 MB comprimidos. Um proxy público típico entrega isso a algumas dezenas de KB/s, o que significa mais de um minuto de janela cinza — e era exatamente esse proxy que a versão anterior aprovava, porque só verificava se ele respondia. Agora cada candidato passa por três etapas: o túnel até `gateway.discord.gg`, que carrega o WebSocket; o HTML do `/app`, que precisa trazer o `GLOBAL_ENV` de verdade e não a página de um bloqueio ou portal de captura; e um pedaço real do bundle, cronometrado, com piso de 200 KB/s. Os aprovados são usados do mais rápido para o mais lento.

Depois da recarga, o programa acompanha o título da janela para saber se o Discord voltou de verdade ou parou na tela cinza; quando para, o proxy vai para `blocked.txt` em `%LOCALAPPDATA%\DiscordRefreshProxy` e a tentativa recomeça com o próximo aprovado (até três por clique). O veto dura sete dias, porque proxy público troca de dono e de rota. Esse julgamento depende de a janela ter um canal aberto no título; sem isso o programa avisa que não consegue julgar a recarga.

O PAC é servido em `127.0.0.1` numa porta efêmera e apontado pelo `AutoConfigURL` do usuário atual (`HKCU`), que é como o Chromium — e portanto o Discord, que é Electron — resolve proxy no Windows. O valor anterior do `AutoConfigURL` é salvo em `%LOCALAPPDATA%\DiscordRefreshProxy` e restaurado ao final; se o programa for encerrado à força, a execução seguinte restaura sozinha.

## Uso

1. Baixe `DiscordRefreshProxy-x64.exe` em **Releases**.
2. Abra o Discord.
3. Execute o programa e clique no botão. Não há prompt de elevação.

## Publicar

```text
git add .
git commit -m "Native Win32 release"
git push origin main
git tag v2.0.0
git push origin v2.0.0
```

O GitHub Actions compila x64 e ARM64 com runtime C++ estático, roda o autoteste (`--selftest`), incorpora o ícone de cadeado aberto e publica os checksums. O executável esperado tem poucos megabytes ou menos; o tamanho exato aparece após o build.

## Fontes externas

- [ProxyScrape Public API](https://docs.proxyscrape.com/api-reference/public-api/get-proxy-list).

Proxies públicos são instáveis e podem observar IP, horário e destinos, embora nenhum certificado seja instalado e o Discord continue usando TLS. A ferramenta altera somente a origem de rede temporária; não modifica dados da conta.

## Limites conhecidos

- A voz do Discord usa UDP e não passa por proxy HTTP: só o gateway e as chamadas HTTPS mudam de origem.
- Enquanto o proxy está ativo, um PAC corporativo eventualmente configurado fica suspenso e volta ao normal ao final.
- Remover a configuração afeta apenas conexões novas; a sessão já aberta segue pelo proxy até reconectar.

## Desenvolvimento

Abra `DiscordRefreshProxy.vcxproj` no Visual Studio com **Desktop development with C++**, ou use MSBuild. O projeto seleciona `v143` no Visual Studio 2022 e `v145` no Visual Studio 2026. O código do projeto usa licença MIT.
