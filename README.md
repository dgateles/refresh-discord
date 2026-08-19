# Discord Refresh Proxy

Aplicativo nativo C++/Win32 de um clique. Não requer .NET, PowerShell ou instalação e não abre console.

Ele encontra um proxy público estrangeiro com HTTPS, encaminha temporariamente apenas o TCP do Discord, envia `Ctrl+R`, mantém o proxy por 45 segundos após a recarga e encerra o túnel. Durante a espera, confirma periodicamente que o túnel continua ativo. O sing-box é baixado separadamente na primeira execução, validado por SHA-256 e armazenado em `%LOCALAPPDATA%\DiscordRefreshProxy`.

## Uso

1. Baixe `DiscordRefreshProxy-x64.exe` em **Releases**.
2. Abra o Discord.
3. Execute o programa, aceite a elevação de Administrador e clique no botão.

## Publicar

```text
git add .
git commit -m "Native Win32 release"
git push origin main
git tag v2.0.0
git push origin v2.0.0
```

O GitHub Actions compila x64 e ARM64 com runtime C++ estático, incorpora o ícone de cadeado aberto e publica os checksums. O executável esperado tem poucos megabytes ou menos; o tamanho exato aparece após o build.

## Fontes externas

- [sing-box](https://github.com/SagerNet/sing-box), baixado em tempo de execução e sujeito à própria licença.
- [ProxyScrape Public API](https://docs.proxyscrape.com/api-reference/public-api/get-proxy-list).

Proxies públicos são instáveis e podem observar IP, horário e destinos, embora nenhum certificado seja instalado e o Discord continue usando TLS. A ferramenta altera somente a origem de rede temporária; não modifica dados da conta.

## Desenvolvimento

Abra `DiscordRefreshProxy.vcxproj` no Visual Studio com **Desktop development with C++**, ou use MSBuild. O projeto não fixa uma versão de toolset: utiliza automaticamente a versão disponível no ambiente de compilação. O código do projeto usa licença MIT.
