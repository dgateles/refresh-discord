# Changelog

## 1.0.2

- Substitui `SendInput` por `keybd_event`, o mesmo metodo que funcionou no prototipo.
- Evita que a recarga seja cancelada quando o Windows bloqueia `SendInput`.

## 1.0.1

- Corrige a ativacao da janela do Discord em sistemas com bloqueio de foco.
- Associa temporariamente as threads de interface antes de enviar `Ctrl+R`.
- Adiciona envio direto para a janela como alternativa, sem cancelar o tunel.

## 1.0.0

- Primeira versao.
