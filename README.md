# Discord Refresh Proxy

Aplicativo Windows de um clique que:

1. encontra e testa um proxy público estrangeiro com HTTPS;
2. encaminha temporariamente apenas o TCP dos processos do Discord;
3. coloca o Discord em primeiro plano e envia `Ctrl+R`;
4. aguarda 15 segundos;
5. encerra o túnel e apaga a configuração temporária.

Não usa WireGuard, não altera região, idade ou pagamento da conta e não mantém o proxy conectado. Nenhuma janela de PowerShell ou terminal é aberta.

## Uso

1. Baixe `DiscordRefreshProxy-win-x64.exe` na página **Releases**.
2. Abra o Discord.
3. Execute o programa e aceite o pedido de Administrador, necessário para o adaptador TUN.
4. Clique em **DESBLOQUEAR DISCORD AGORA**.

Na primeira execução, o programa baixa o release estável oficial do [sing-box](https://github.com/SagerNet/sing-box), valida o SHA-256 publicado pelo GitHub e o guarda em `%LOCALAPPDATA%\DiscordRefreshProxy`. A lista de proxies vem da [API pública do ProxyScrape](https://docs.proxyscrape.com/api-reference/public-api/get-proxy-list).

## Publicar no GitHub

Crie um repositório e envie estes arquivos. Para gerar uma release:

```text
git tag v1.0.0
git push origin v1.0.0
```

O workflow compila executáveis únicos e autocontidos para Windows x64 e ARM64, gera seus SHA-256 e cria a release automaticamente. Também é possível executar **Build release** manualmente na aba Actions; nesse caso os executáveis ficam em Artifacts, sem criar uma release.

## Observações

- Proxies públicos são instáveis; o aplicativo testa vários automaticamente.
- Nenhum certificado de proxy é instalado. O Discord continua usando TLS.
- O operador do proxy pode observar IP, horário e destinos, ainda que não veja o conteúdo TLS.
- O desbloqueio só altera temporariamente a origem de rede/IP. Restrições vinculadas ao backend da conta podem permanecer.
- Os logs locais do sing-box ficam em `%LOCALAPPDATA%\DiscordRefreshProxy\logs`.

## Desenvolvimento

Requer .NET 8 SDK no Windows:

```text
dotnet run
```

Licença MIT. O sing-box é baixado separadamente em tempo de execução e mantém sua própria licença.
