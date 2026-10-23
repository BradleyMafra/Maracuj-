# Maracujá no Windows e Linux

O Maracujá distribui pacotes para Windows e Linux, com runtimes privados. Os scripts compilam e executam os testes antes de gerar os downloads. O gesto no Explorador e o uso de múltiplos monitores requerem conferência interativa no Windows.

Baixe os pacotes da versão **0.9.5** em [Releases](https://github.com/BradleyMafra/Maracuj-/releases): `Maracuja-0.9.5-Setup-x64.exe` ou `Maracuja-0.9.5-windows-x64-portable.zip`, acompanhados dos arquivos SHA256. O mesmo lançamento inclui o TAR e o AppImage Linux.

O código é disponibilizado sob [MIT](LICENSE). Os componentes dos pacotes mantêm suas licenças e avisos. Para compilar localmente no Windows, não é necessário habilitar o GitHub Actions nem instalar ferramentas globais.

## Compilar no Windows com um comando

1. No Windows 10 (1903 ou posterior) ou Windows 11 **x64**, entre na sua conta do GitHub e abra [BradleyMafra/Maracuj-](https://github.com/BradleyMafra/Maracuj-).
2. Use **Code → Download ZIP** e extraia a pasta inteira para um caminho curto, por exemplo `C:\Projetos\Maracuja`. Não execute o script dentro do ZIP.
3. Abra `build-windows.cmd` na raiz da pasta. Também pode executar `.\build-windows.cmd` no terminal dessa pasta.

Se já trabalha com um clone Git no Windows, confira o trabalho local e traga as mudanças mais recentes antes de compilar. Com o checkout limpo e sem divergência, use `git pull --ff-only origin main`; integre alterações locais antes de atualizar quando necessário. Não substitua um clone com mudanças ainda não enviadas por um ZIP antigo.

O script usa o PowerShell e o `tar.exe` já presentes no Windows 10/11. Ele baixa versões exatas de Python, CMake, Ninja, Qt 6.8.3 e MinGW 13.1, verifica seus SHA256 e prepara tudo em `.build-tools` e `.package-cache`, dentro do projeto. Não exige Visual Studio, Python, Qt, 7-Zip ou Office instalados globalmente e não adiciona inicialização automática. A primeira execução precisa de internet e de vários GiB livres para downloads, ferramentas, compilação e pacote; as seguintes reaproveitam o cache verificado.

Pastas com acentos, como `Maracujá`, são aceitas pelo comando principal: o build cria uma unidade temporária com `subst` para as ferramentas Qt/MinGW que não interpretam esses caminhos corretamente. Os arquivos continuam na pasta original e o mapeamento é removido ao terminar. É preciso haver uma letra de unidade livre. O Qt é extraído diretamente dos arquivos verificados, sem um segundo download nem a consulta de `qmake` que falhava com a codificação do Windows.

A compilação e os testes executam sequencialmente. O processo monta a distribuição completa e testa conversões de imagens, documentos, PDF, áudio e OCR antes de produzir:

- `dist\artifacts\Maracuja-0.9.5-Setup-x64.exe`
- `dist\artifacts\Maracuja-0.9.5-windows-x64-portable.zip`
- Arquivos `.sha256` correspondentes.

O registro fica em `validation\build-windows-AAAAmmdd-HHMMSS.log`. Se houver erro, o script para sem anunciar sucesso: envie o trecho final desse log para revisão. Reexecute o mesmo comando depois da correção; ele recria apenas o staging marcado como pertencente a este projeto e conserva o cache de downloads.

O log identifica as etapas, os downloads e os comandos executados. Extrações interrompidas de Python, CMake e Ninja são refeitas na próxima execução; a conclusão é registrada somente após a validação das ferramentas. A configuração do CMake é renovada quando o caminho muda ou há caminhos inválidos no cache; nas demais execuções, a compilação é incremental. Os atributos do Git preservam os bytes dos arquivos de OCR, inclusive a licença, para que a conversão automática de quebras de linha do Windows não invalide seus hashes.

O iniciador também seleciona os módulos internos do Windows PowerShell, evitando o erro de `Get-FileHash` ou `Expand-Archive` não encontrado quando o `.cmd` é aberto a partir do PowerShell 7. Essa seleção vale somente para o processo de build, sem alterar as configurações do terminal.

O build local não publica nada no GitHub. Os arquivos resultantes podem ser anexados a um release depois da validação. Os testes das ferramentas são executados contra o helper que acompanha o pacote. As correções anteriores de ZIP Forte e EXIF/GPS em TIFF foram preservadas. O build configura as fontes instaladas no Windows para os testes QML sem interface, garantindo medidas reais de texto no editor compacto; o teste de limite de memória exclusivo do POSIX é ignorado no Windows. Os testes automáticos devem ser complementados pela verificação interativa do arraste real no Explorer e das janelas em múltiplos monitores.

## Conversão e edição

O código Windows oferece dois gestos independentes com o Maracujá em execução:

- **Shift + arrastar:** abre a roda de conversão. Em **Atalho → Conversão**, é possível escolher outra combinação ou abrir a roda sem arraste, no modo **Somente teclas**.
- **Alt + arrastar:** abre a roda com **Comprimir**, **Metadados** e, para uma única imagem compatível, **Editar foto**. Soltar em uma opção apresenta a janela independente; confirme a operação no botão **Comprimir** ou **Salvar cópia**. Em **Atalho → Edição**, escolha outra tecla ou combinação; esse segundo gesto sempre requer arraste.

Na janela do código 0.9.5, use o seletor **Conversão / Edição**, junto ao título **Escolha o próximo formato**. **Conversão** mantém os formatos disponíveis; **Edição** mostra **Comprimir**, **Metadados** e **Editar foto**. A lista de arquivos é compartilhada entre os modos e aceita tipos sem conversão disponível quando adicionados em Edição. Clique ou solte arquivos sobre uma ação para abrir sua janela de opções; isso não grava arquivos sem confirmação.

O editor de fotos oferece prévia ao vivo, comparação com o original e 18 ajustes de luz, cor e detalhes, incluindo remoção de névoa, clareza, granulação e redução de ruído. Exporta uma cópia PNG, JPG ou WebP na resolução original, respeitando o destino configurado ou a pasta da foto. Aceita uma foto estática de até 24 megapixels/128 MiB, em um fluxo sRGB de 8 bits. O processamento usa o Qt já incluído no aplicativo, sem novo download de ferramentas. Essa função está incluída no pacote Windows 0.9.5.

A seleção aceita até 128 arquivos regulares locais; a compressão admite até 512 MiB por arquivo e 2 GiB no lote. **Automático** otimiza JPEG, PNG, WebP e PDF; os demais tipos são agrupados em ZIP. A opção **Arquivo ZIP** reúne a seleção com nomes únicos. Os perfis Equilibrado/Forte e o redimensionamento ajustam a compressão; metas em MB ou porcentagem de redução são tentativas, e o painel mostra o tamanho real e se a meta foi atingida. Formatos com perdas podem reduzir a qualidade.

O painel de metadados permite buscar, editar ou remover campos suportados de JPEG, PNG, WebP, TIFF de uma página, PDF, DOCX, XLSX e PPTX, com até 128 MiB por arquivo. Campos técnicos ficam somente leitura; orientação e perfis de cor necessários à aparência são mantidos. Outros tipos exibem informações básicas em modo de leitura. A remoção de metadados não remove conteúdo visível, comentários, anexos ou histórico de revisões do documento. Todas as operações geram novas cópias com nomes únicos e preservam os originais.

## Windows x64

Requer Windows 10 versão 1903 ou posterior, ou Windows 11. As ferramentas de PDF recebem um manifesto com `activeCodePage=UTF-8`, preservando os demais recursos do executável, para funcionar em pastas com acentos sem mudar a configuração regional do sistema. Esse recurso está disponível a partir do Windows 10 1903 ([documentação Microsoft](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page)). Os testes de conversão usam o caminho físico do pacote, mesmo quando a compilação usa a unidade temporária.

Use `Maracuja-0.9.5-Setup-x64.exe` do release ou compile o código atual com `build-windows.cmd`. A instalação acontece no usuário atual, sem administrador. O instalador oferece opções para iniciar em segundo plano, adicionar o menu “Converter com Maracujá” e criar um atalho. O início automático vem desmarcado. Não altera os aplicativos padrão dos arquivos. Ao atualizar, solicita o encerramento cooperativo apenas da instância do próprio Maracujá.

O menu de contexto clássico recebe **um arquivo por invocação**. Para converter vários arquivos juntos, use a seleção múltipla na janela ou o gesto com Shift durante o arraste.

O ZIP portátil contém o mesmo aplicativo e os runtimes privados. Extraia **toda a pasta** e execute `maracuja.exe`; não copie apenas o EXE. A distribuição inclui Qt, Python, LibreOffice, FFmpeg, Poppler e OCR português/inglês, sem depender do Codex, Python ou Office instalados no computador.

## Linux x86-64

Baseline do pacote: Ubuntu 24.04/glibc 2.39 ou posterior. O gesto com Shift requer KDE Plasma 6/Wayland. Outros ambientes podem usar a janela de conversão; não se presume que ofereçam a integração específica do KWin.

Extraia o `.tar.gz` e execute `AppRun`. Para instalar no usuário atual, execute `sh install-user.sh`; acrescente `--autostart` somente se quiser o início em segundo plano. A instalação registra o menu do Dolphin e o efeito do próprio Maracujá, preservando temas, painéis e associações de arquivos.

No AppImage, dê permissão de execução e abra normalmente. Para habilitar a integração do KWin, execute uma vez `./Maracuja-0.9.5-linux-x86_64.AppImage --install-integration` e reabra o aplicativo, ajustando o nome à versão do pacote obtido ou compilado. O carregamento normal de AppImage pode exigir FUSE; `--appimage-extract-and-run` é a alternativa do runtime em sistemas sem FUSE.

## Compilação avançada e CI

Os scripts verificam os SHA256 travados e executam compilação/testes sequencialmente. Recusam staging existente sem a marca de propriedade correspondente. Os comandos de sistema do workflow são destinados aos runners descartáveis do GitHub.

O caminho recomendado no Windows é `build-windows.cmd`. Para quem já tem um ambiente MSVC x64, CMake 3.24 ou posterior e Qt 6.8.3 compatíveis em um caminho sem acentos, permanece disponível o empacotador direto:

```powershell
./packaging/windows/build.ps1 -Version 0.9.5 -QtRoot C:\Qt\6.8.3\msvc2022_64
```

Linux, com as dependências de compilação listadas no workflow:

```sh
python3 -I packaging/linux/build.py --version 0.9.5 --appimage
```

O workflow opcional **Build and validate releases** depende de o GitHub Actions estar disponível na conta. Quando executado com `workflow_dispatch` sobre uma branch, gera artefatos de validação sem publicar um release. Tags `vX.Y.Z` precisam coincidir com a versão em CMake e publicam Linux e Windows somente depois que os dois jobs passam. Uma execução manual sobre uma tag `vX.Y.Z` também pode publicar o release.

O lançamento 0.9.5 reúne as duas plataformas. Uma publicação manual anexa somente os pacotes e checksums validados, ligados ao mesmo commit. A compilação local funciona independentemente do GitHub Actions. Confira nas notas do release o ambiente de compilação e os testes realizados; uma execução via Wine não substitui a conferência interativa do desktop Windows.

As versões/SHA256 de Python, LibreOffice, ferramentas MinGW e wheels ficam em `packaging/*/*.lock*`. Se um servidor remover um arquivo travado, o build falha; não substitui a dependência por uma versão diferente silenciosamente. Licenças, inventário de arquivos e referências às fontes correspondentes acompanham o pacote em `licenses/`.
