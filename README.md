# Maracujá

Conversor local de imagens, PDFs, documentos e áudio para **Linux e Windows**, com interface Qt Quick e núcleo C++/Qt. Por padrão, **Shift + arrastar** abre a roda de conversão e **Alt + arrastar** abre as ferramentas **Comprimir**, **Metadados** e **Editar foto**. Os dois atalhos são configuráveis. Versão 0.9.5.

## Baixar

Os pacotes ficam em [Releases](https://github.com/BradleyMafra/Maracuj-/releases). A versão **0.9.5** reúne Linux e Windows no mesmo lançamento.

- **Windows x64:** baixe `Maracuja-0.9.5-Setup-x64.exe` para instalar por usuário ou `Maracuja-0.9.5-windows-x64-portable.zip` para extrair e executar. As dependências de conversão acompanham o pacote; não é necessário instalar Python ou LibreOffice globalmente. Para compilar, execute `build-windows.cmd`; veja [README_WINDOWS.md](README_WINDOWS.md).
- **Linux x86_64:** baixe `Maracuja-0.9.5-linux-x86_64.tar.gz` ou `Maracuja-0.9.5-linux-x86_64.AppImage`. A base é Ubuntu 24.04/glibc 2.39 ou mais recente. Os gestos globais requerem KDE Plasma 6 em Wayland e a integração incluída; a janela e as conversões também podem ser usadas sem o gesto.

Os pacotes acompanham checksums SHA256, inventário e avisos das dependências. O código está sob MIT; os componentes de terceiros conservam suas licenças. Os scripts permitem compilar localmente, e o GitHub Actions é uma alternativa opcional. Configurações pessoais, credenciais e arquivos usados nas conversões não fazem parte dos downloads.

## Usar

- Abra **Maracujá** pelo menu de aplicativos e adicione arquivos, ou arraste os arquivos diretamente sobre um formato da roda.
- Com o Maracujá em execução no KDE Wayland, comece a arrastar um arquivo compatível e **depois toque Shift**, o atalho padrão. A roda aparece sob o cursor e oferece os formatos adequados à seleção: arraste até o formato e solte. Pode soltar Shift antes de escolher, ou mantê-lo pressionado. Soltar no centro ou sair da roda cancela. A janela principal pode ficar escondida em segundo plano. O botão **Atalho** permite personalizar esse gesto.
- A roda flutuante de conversão usa apenas os arquivos do arraste. Abrir, cancelar ou concluir esse gesto preserva os arquivos, a ordem dos formatos e o formato selecionado no painel principal, tanto no Linux quanto no Windows. O progresso e os resultados da conversão continuam disponíveis no painel.
- Para editar, comece a arrastar um arquivo e **depois toque Alt**. Solte em **Comprimir**, **Metadados** ou **Editar foto** para abrir uma janela independente. O drop só abre as opções: confirme a operação na janela. **Editar foto** aparece no gesto quando há uma única imagem compatível. Na janela principal, selecione **Edição** em **Escolha o próximo formato** para ver as três ações.
- No modo em segundo plano, use o ícone do Maracujá na bandeja para **Abrir Maracujá** ou **Sair**. Fechar a janela pelo **X** apenas a esconde e mantém o gesto disponível; isso não cancela uma conversão em andamento. **Sair** encerra o aplicativo e o gesto nesta sessão, cancelando antes uma conversão que ainda esteja em andamento.
- No Dolphin, selecione imagens, PDFs, documentos ou áudios e use **Converter com Maracujá…** no menu do botão direito (pode aparecer em **Ações**). Isso abre a roda compacta já com a seleção. Separe essas categorias em conversões diferentes.
- No seletor **Conversão / Edição**, **Conversão** mostra os formatos suportados e **Edição** mostra **Comprimir**, **Metadados** e **Editar foto**. A lista de arquivos é compartilhada entre os modos; em Edição, também aceita tipos sem conversão disponível. Com arquivos já selecionados, clique em um formato para converter imediatamente. `Ctrl+O` adiciona arquivos; `Enter` converte somente no modo Conversão; `Esc` cancela uma conversão em andamento.
- O destino padrão é a pasta de cada original. Ao reunir vários arquivos em um PDF, o resultado vai para a pasta do primeiro arquivo. Também é possível escolher outra pasta.

As conversões criam novos arquivos. Nomes já existentes recebem um sufixo numérico; os originais não são sobrescritos. Ao converter PDFs ou documentos para PNG/JPEG, as imagens são salvas diretamente na pasta de destino, sem criar subpastas. Uma página usa `nome-convertido.png`; documentos com várias páginas usam `nome-convertido-pagina-1.png`, `nome-convertido-pagina-2.png` e assim por diante (com a extensão correspondente). O aplicativo não envia arquivos à internet nem coleta telemetria.

A roda combina violeta e dourado, com abertura suave e realce animado do formato sob o cursor. O ícone representa um maracujá inteiro e sua metade aberta.

## Configurar atalhos

Abra **Atalho** no cabeçalho do painel, ou **Configurar atalhos…** no menu `⋯`. As abas **Conversão** e **Edição** possuem configurações independentes. Na aba **Conversão**, escolha como abrir a roda:

- **Com arraste:** comece a arrastar o arquivo e pressione a tecla ou combinação configurada. Shift continua sendo o padrão; também é possível usar outro modificador ou uma combinação, como Ctrl+Alt+M. A roda fica disponível até soltar o arquivo.
- **Somente teclas:** pressione uma combinação, como Ctrl+Alt+M, ou uma tecla de função disponível, como F9, para abrir a roda compacta. Ela usa os arquivos já adicionados ao painel; quando estiver vazia, permite escolher arquivos. Clique em um formato para converter.

Na aba **Edição**, configure a tecla ou combinação do segundo gesto. Ele sempre requer arraste e usa **Alt** por padrão, mesmo quando a conversão está no modo **Somente teclas**. Os dois gestos precisam ter atalhos diferentes.

Clique no campo **Novo atalho**, pressione as teclas e solte para concluir a gravação. A captura acontece apenas nesse campo, enquanto ele está focado. Confira a combinação exibida e clique em **Salvar**. **Cancelar** descarta o rascunho. **Usar Shift** prepara novamente o padrão de conversão; **Usar Alt** prepara o padrão das ferramentas. Confirme em **Salvar** para restaurá-lo. As preferências ficam salvas para as próximas execuções do aplicativo.

Modificadores sozinhos, como Shift, Ctrl ou Alt, são aceitos no modo com arraste. O modo somente teclas exige uma tecla além dos modificadores, ou uma tecla de função. Letras e números precisam incluir **Ctrl, Alt ou Meta**; uma letra sozinha ou apenas com Shift não é aceita. As teclas de função F1 a F12 dependem da disponibilidade no sistema. No Windows, Meta corresponde à tecla Windows.

Combinações com tecla e teclas de função ficam reservadas enquanto o atalho estiver ativo e o Maracujá estiver em execução. Escolha uma combinação que não seja usada por outro aplicativo ou pelo ambiente. Quando o sistema informa um conflito, a configuração exibe o erro e preserva o atalho anterior. A ativação do próprio Maracujá fica suspensa enquanto o diálogo de configuração está aberto, para permitir gravar inclusive o atalho atual.

No Linux, a integração global requer **KDE Plasma em Wayland**; em outros ambientes, a janela de conversão continua disponível. No Windows, os testes automáticos do pacote precisam ser complementados pela verificação interativa do gesto no Explorador e dos atalhos globais. Veja [README_WINDOWS.md](README_WINDOWS.md).

## Aparência da roda

Abra **⋯ → Aparência da roda** no painel. Escolha uma das cores prontas, use **Escolher cor** para abrir o seletor ou informe um código **#RRGGBB**. A cor tinge os fundos e as bordas; o maracujá e o destaque dourado preservam suas cores. Ajuste também a transparência de **0%** (fundo opaco) a **80%** (mais transparente) e o tamanho de **70%** a **130%**, acompanhando a prévia. O switch **Som da roda** ativa ou desativa um toque curto e suave ao entrar em uma opção, na janela e nas rodas flutuantes de conversão e edição. O som vem ativado e limita repetições durante movimentos rápidos. **Restaurar padrão** prepara lilás, transparência de **35%**, tamanho de **100%** e som ativado. Confirme em **Salvar** para aplicar todas as preferências; elas ficam salvas para as próximas sessões no mesmo sistema. **Cancelar** ou `Esc` descartam o rascunho. Nomes e ícone acompanham a escala e continuam definidos. Um tamanho salvo durante um arraste passa a valer ao abrir a próxima roda, preservando a seleção em andamento.

O áudio é gerado localmente, sem downloads ou novos processos a cada movimento. No Linux, usa o serviço de áudio do sistema via `libcanberra.so.0` (pacote `libcanberra0` em Ubuntu/KDE neon); se o backend não estiver disponível, a roda continua funcionando em silêncio. No Windows, usa o áudio nativo do sistema.

## Pasta de saída

Sem uma pasta escolhida em **Salvar em**, cada cópia é criada ao lado do arquivo original. Essa é a opção inicial; a pasta de um arquivo não vira um destino fixo para os próximos. Uma saída que reúne vários arquivos, como PDF ou ZIP, fica ao lado do primeiro original.

A pasta escolhida no painel tem prioridade durante a execução do aplicativo e vale para conversão, compressão, metadados e fotos editadas, inclusive nos gestos. **Local original** restaura o padrão. Ao abrir **Escolher pasta**, o seletor começa no destino escolhido ou, na ausência dele, na pasta do primeiro arquivo selecionado. Só inspecionar metadados ou abrir o editor não cria cópias.

## Comprimir arquivos

Selecione **Edição → Comprimir** na roda, ou arraste arquivos, toque **Alt** e solte em **Comprimir**. A seleção aceita até **128 arquivos locais regulares e legíveis**, com até 512 MiB por arquivo e 2 GiB no lote; pastas não são tratadas como arquivos compactáveis nesta versão.

- **Automático:** otimiza JPEG, PNG, WebP e PDF no mesmo formato. Os demais tipos são agrupados em ZIP.
- **Arquivo ZIP:** reúne qualquer seleção aceita em um ZIP. Os nomes originais são preservados; nomes iguais recebem um sufixo para permanecerem únicos dentro do arquivo.
- **Equilibrado/Forte:** escolhe a intensidade da compressão. JPEG, WebP e imagens de PDF podem perder qualidade; o perfil forte prioriza tamanho menor.
- **Dimensões:** permite manter as imagens ou limitar seu maior lado a 2048 ou 1280 pixels.
- **Meta:** tenta atingir um tamanho em MB ou uma porcentagem de redução. O painel informa o tamanho original, o resultado e se a meta foi atingida. O conteúdo e o formato podem impedir a redução solicitada; ZIP e arquivos já compactados podem ficar maiores.

A janela compacta mostra os perfis **Equilibrado/Forte**, o tipo de saída e as dimensões. Abra **Mais opções** para definir uma meta de tamanho ou de redução.

Clique em **Comprimir** para iniciar. O resultado é uma nova cópia, com nome único, na pasta de saída escolhida ou ao lado do original quando não houver destino personalizado. Uma seleção destinada a ZIP produz um arquivo com todos os itens, sem substituir os arquivos de origem.

## Inspecionar e editar metadados

Selecione **Edição → Metadados** na roda, ou arraste o arquivo, toque **Alt** e solte em **Metadados**. Escolha o arquivo da seleção e use a busca para localizar campos e grupos. A inspeção apresenta os valores existentes e identifica os campos que podem ser editados ou removidos.

Os campos editáveis ficam à vista e **Detalhes** expande as informações técnicas. Formatos sem metadados editáveis, como TXT, mostram apenas informações de leitura.

A edição oferece suporte a **JPEG, PNG, WebP, TIFF de uma página, PDF, DOCX, XLSX e PPTX**, com limite de 128 MiB por arquivo. Campos técnicos, como formato e dimensões, permanecem somente leitura. É possível alterar campos editáveis, marcar informações removíveis individualmente ou usar **Remover todos os metadados removíveis na cópia**. Orientação e perfis de cor necessários à aparência são mantidos. Tipos sem suporte de edição apresentam apenas as informações básicas do arquivo, em modo de leitura.

**Salvar cópia** aplica o rascunho a um novo arquivo; o original não é sobrescrito. A remoção abrange os campos de metadados suportados. Ela não remove conteúdo visível, comentários, anexos ou histórico de revisões incorporado ao documento.

## Editar fotos

Selecione **Edição → Editar foto**, ou arraste uma única foto, toque **Alt** e solte em **Editar foto**. Sem arquivos selecionados, clicar nessa ação permite escolher uma foto. O editor abre em uma janela flutuante independente, preservando o painel principal e eventuais rascunhos de metadados.

- **Luz:** exposição, brilho, contraste, realces, sombras, brancos, pretos e gama.
- **Cor:** temperatura, matiz verde/magenta, saturação, vivacidade e tonalidade.
- **Detalhes:** clareza, remoção de névoa, nitidez, redução de ruído e granulação.

Os controles atualizam uma prévia de até 1280 pixels no maior lado. Segure **Original** para comparar, restaure um controle individual ou use **Restaurar** para voltar aos ajustes neutros. As setas do teclado permitem ajustes finos nos seletores. A prévia e a exportação usam o mesmo processamento local; redução de ruído, nitidez e granulação podem parecer diferentes na imagem em resolução completa.

**Salvar cópia** exporta PNG, JPG ou WebP, conforme os codificadores disponíveis, com as dimensões originais e nome `foto-editado.ext`. Colisões recebem um número, sem substituir arquivos existentes. O destino é o configurado no painel ou, por padrão, a pasta da foto original. JPG aplica fundo branco onde houver transparência. Fechar com ajustes não salvos pede confirmação; cancelar a exportação preserva o original.

Esta versão edita uma imagem estática por vez, de até **24 megapixels** e **128 MiB**, em sRGB de 8 bits por canal, aplicando a orientação da foto. Não é um fluxo RAW/HDR nem conserva a profundidade de cor de imagens de 16 bits. Animações, documentos e imagens com várias páginas são recusados. Não há dependência de um serviço externo nem envio da foto à internet. O editor está incluído no pacote Windows 0.9.4.

## Conversões desta versão

- Imagens → JPG, PNG, WebP, BMP, TIFF e AVIF, conforme os codificadores disponíveis no computador.
- Imagens → TXT ou Word (`.docx`), com reconhecimento local de texto (OCR) em português e inglês.
- Fotos → um PDF A4, com uma imagem por página e orientação automática.
- PDF → PNG/JPG, incluindo todas as páginas.
- PDF → TXT, DOCX, Markdown, HTML ou EPUB. O texto existente é preservado na extração; nas páginas sem texto selecionável, o OCR local reconhece o conteúdo em português e inglês.
- PDF → PPTX, com uma imagem por página/slide. O visual da página é preservado, mas seus elementos não viram objetos editáveis.
- PDF → XLSX, extraindo somente tabelas detectadas. Quando não houver tabela, a operação informa isso em vez de produzir uma planilha artificial.
- Vários PDFs → um PDF.
- TXT, Markdown (`.md`/`.markdown`), HTML (`.html`/`.htm`), DOCX, ODT e RTF → PDF, DOCX, ODT, RTF, TXT, Markdown, HTML ou EPUB. Cada documento produz sua própria saída.
- Documentos → PNG/JPG, com uma imagem por página após a paginação do documento.
- Áudio → MP3, M4A, WAV, FLAC, OGG, OPUS, AIFF e WMA, conforme os codificadores locais disponíveis. Cada áudio produz sua própria saída.

HEIC/HEIF podem ser lidos quando o plugin instalado permite. A saída HEIC só aparece se houver um codificador disponível. Imagens animadas e TIFFs com várias páginas são recusados para evitar conversões incompletas.

A roda mostra somente os destinos disponíveis para a seleção e para as ferramentas detectadas neste computador. A extensão do arquivo não garante uma conversão válida: o conteúdo também é verificado. Documentos de formatos diferentes podem ter diferenças de fonte, paginação ou estrutura após a conversão. O aplicativo não promete uma cópia visual idêntica entre formatos editáveis.

PDF é um formato de página pronta. A conversão para texto ou Word editável reorganiza o texto e não promete reproduzir a diagramação original. O OCR é automático nas páginas sem texto selecionável; páginas com texto existente usam a extração direta. Ele pode confundir letras, números, acentos, colunas ou a ordem de leitura, especialmente em fotos desfocadas ou inclinadas. Revise o resultado antes de usar. Tudo é processado neste computador, sem envio de imagens ou PDFs a serviços externos.

Para usar OCR, arraste a imagem ou PDF, toque **Shift** ou o atalho configurado e solte em **TXT** ou **DOCX**; a mesma função está disponível pela janela, pelo atalho sem arraste e pelo menu do Dolphin. Em PDFs, Markdown, HTML e EPUB também usam OCR quando uma página não contém texto. No PPTX, cada página continua sendo uma imagem. O XLSX extrai tabelas detectadas em PDFs com texto; o OCR não reconstrói planilhas a partir de páginas escaneadas. EPUB é um destino de conversão; EPUB e Apple Pages não são aceitos como entrada nesta versão. Não há exportação Apple Pages.

As conversões de áudio usam os padrões do aplicativo. Formatos com perdas podem reduzir a qualidade; converter um MP3 para FLAC, por exemplo, não recupera informações já perdidas. O controle de qualidade se aplica às saídas de imagem. A resolução em DPI é usada ao transformar páginas de PDFs ou documentos em imagens; esses controles não alteram áudio nem o reconhecimento OCR.

## Integração com o Windows

Com o Maracujá aberto, comece a arrastar um arquivo no Explorador e toque Shift, ou o atalho configurado no modo com arraste. A roda flutuante oferece os formatos da seleção; solte sobre um deles para converter. **Alt + arrastar** abre a segunda roda, com **Comprimir**, **Metadados** e, para uma única imagem compatível, **Editar foto**. O drop abre somente uma janela flutuante de opções, sem revelar ou restaurar o painel principal. O destino OLE aceita somente arquivos locais e negocia **cópia**, inclusive com Shift pressionado. A roda não toma o foco do Explorador; depois do drop, a janela de opções aceita edição pelo teclado. A abertura usa uma animação curta violeta e dourada. O modo **Somente teclas** abre a janela compacta com os arquivos do painel; o gesto das ferramentas continua independente.

O release Windows **0.9.4** foi compilado no Windows e passou por **181 verificações automáticas**, além da revisão de 64 capturas em escalas de 100% e 150%. A validação inclui o editor de fotos, os modos da roda e o tamanho/transparência da roda nativa. O build configura as fontes Windows para os testes sem interface. Um teste exclusivo de POSIX é ignorado por não se aplicar ao Windows. O arraste físico no Explorer e o movimento entre múltiplos monitores ainda precisam de conferência manual.

O instalador funciona por usuário, sem exigir administrador. As opções de iniciar em segundo plano e adicionar **Converter com Maracujá** ao menu de contexto são apresentadas na instalação. No Windows 11, o comando pode aparecer em **Mostrar mais opções**. A autoinicialização é opcional. A versão portátil não altera o registro automaticamente.

`maracuja.exe --background` mantém somente o ícone na bandeja. Abrir outra vez reutiliza a instância do mesmo usuário por um canal local com acesso restrito. Fechar pelo X esconde a janela; **Sair do Maracujá** cancela a conversão ativa e encerra o gesto.

Os conversores executam em um Job Object exclusivo, criado antes de o primeiro subprocesso começar. O cancelamento encerra essa árvore e seus temporários. Job Objects controlam processos; não são uma sandbox de arquivos. No Windows, a preparação de documentos recusa macros, recursos externos e alguns conteúdos ativos, e o LibreOffice usa um perfil temporário com macros e atualizações desabilitadas. A política é mais conservadora do que a importação comum do Office e pode recusar documentos complexos.

## Integração com o Linux

A opção **Ativo** controla a integração com o KDE Wayland; o botão **Atalho** configura as combinações independentes de conversão e ferramentas. No gesto com arraste, o Maracujá carrega um pequeno efeito do KWin durante sua execução e o descarrega ao sair. O efeito acompanha os modificadores configurados e o botão esquerdo para posicionar exclusivamente o alvo próprio de cada roda; ela só é desenhada depois que recebe um arraste válido de arquivos locais. Combinações com tecla usam o serviço de atalhos do KDE para ativar o gesto ou abrir a roda de conversão sem arraste. O aplicativo registra somente as combinações escolhidas: não lê texto digitado em outros programas, clipboard ou histórico, nem altera o Deskflow.

A roda de arraste usa um destino Wayland que anuncia exclusivamente **cópia**, inclusive quando Shift continua pressionado. O trabalho de conversão e os resultados continuam na janela do Maracujá. Um aviso do desktop informa a conclusão. Abrir novamente pelo Dolphin usa a instância existente.

`maracuja --background` inicia sem mostrar a janela principal, mantendo o ícone na bandeja e o gesto disponível. Abrir o Maracujá pelo menu ou pelo Dolphin mostra a mesma instância; uma nova chamada a `--background` sem arquivos não traz a janela à frente. A autoinicialização no KDE é opcional e usa esse modo. Ela é instalada somente com a opção explícita `--enable-autostart` do instalador. Para desativar a próxima inicialização automática, remova ou desative **Maracujá** nas configurações de **Início automático** do KDE. **Sair** pela bandeja encerra apenas a execução atual.

O [Tangerine para macOS](https://tangerineformac.com/) inspirou o visual e a interação com a roda. O Maracujá foi desenvolvido de forma independente, sem acesso ou reutilização do código-fonte do Tangerine e sem afiliação com seus criadores. Veja [Créditos e inspiração](CREDITS.md). A integração usa o [formato oficial de menus do Dolphin](https://develop.kde.org/docs/apps/dolphin/service-menus/).

## Recursos e limites

Uma conversão é executada por vez, fora da thread da interface. O limite por imagem é de 64 megapixels e 256 MiB de imagem decodificada. Páginas de PDF têm até 4096 pixels no maior lado; o DPI escolhido pode ser reduzido em páginas maiores. Documentos de origem têm limite de 64 MiB; os novos destinos editáveis, slides e tabelas a partir de PDF aceitam até 200 páginas. O OCR usa uma página por vez, imagens normalizadas com até 16 megapixels e prazo de 45 segundos por reconhecimento. A seleção da interface aceita até 128 arquivos. O cancelamento encerra somente subprocessos criados para a conversão e remove os temporários da operação em andamento; resultados anteriores já concluídos são preservados.

## Compilar e instalar para o usuário

Dependências de compilação Linux: Qt6 Core/Gui/Widgets/Qml/Quick/QuickControls2/Concurrent/DBus/Network, Wayland Client/Protocols, Cairo/Pango, GLib, CMake e Ninja. No Windows, `build-windows.cmd` prepara Qt 6.8.3 e MinGW privados; também há suporte a MSVC 2022. Veja [README_WINDOWS.md](README_WINDOWS.md).

Os scripts em `packaging/` montam os pacotes com versões e hashes fixados de Python, LibreOffice, Poppler, FFmpeg e OCR. Os caminhos ficam em `runtime-paths.json`, relativos ao próprio pacote. Nenhum caminho de uma instalação do Codex é necessário. Os modelos oficiais `por` e `eng` do tessdata_fast 4.1.0 acompanham origem, hashes e licença Apache 2.0.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 1
```

Para instalar um pacote Linux pronto, extraia-o e execute o `install-user.sh` incluído. Para desenvolvimento em uma instalação já existente, `python3 install-user.py --no-build` atualiza somente os arquivos do Maracujá e reutiliza o runtime privado validado. Em uma instalação nova, use o pacote completo ou forneça explicitamente `--runtime-source`.

A integração fica em `~/.local`, sem trocar associações padrão, temas, painéis ou configurações de outros aplicativos. A inicialização automática é opcional. No instalador de desenvolvimento, use `--enable-autostart`; omitir essa opção preserva a escolha anterior.

O nome visível é **Maracujá** e o comando é `maracuja` (`maracuja.exe` no Windows). A atualização preserva as preferências e os lançadores já fixados da instalação anterior.

A opção de início automático altera somente a entrada pertencente ao Maracujá, guardando uma cópia da anterior quando existir. Reinstalar sem essa opção preserva a escolha anterior. Os demais aplicativos de inicialização não são alterados.

## Licença

O código do Maracujá usa a [licença MIT](LICENSE). Você pode usar, modificar e distribuir o aplicativo mantendo o aviso de licença. Os componentes incluídos nos pacotes conservam suas próprias licenças; veja [Componentes de terceiros](THIRD_PARTY_NOTICES.md) e a pasta `licenses/` de cada pacote.

## Apoio opcional

Se o Maracujá te ajuda e você quiser apoiar o projeto, há um [QR code Pix de valor livre](SUPPORT.md). No aplicativo, abra **⋯ → Apoiar o Maracujá**. O apoio é voluntário; o aplicativo continua completo e gratuito.

## Validar

```sh
python3 tests/integration_check.py build/maracuja
ctest --test-dir build --output-on-failure -j1
python3 tests/wayland_integration_check.py
python3 tests/wayland_integration_check.py --tap-shift
python3 tests/wayland_integration_check.py --document
python3 tests/wayland_tools_check.py --tool compress --input-kind image
python3 tests/wayland_tools_check.py --tool metadata --input-kind image
python3 tests/wayland_tools_check.py --tool compress --input-kind binary
python3 tests/wayland_tools_check.py --tool photo --input-kind image
# Com o Python privado do pacote e o caminho do runtime do mesmo pacote:
export MARACUJA_RUNTIME_CONFIG="/caminho/Maracuja/runtime-paths.json"
runtime/python/bin/python3 -I tests/expanded_formats_check.py build/maracuja
runtime/python/bin/python3 -I tests/ocr_check.py build/maracuja
runtime/python/bin/python3 -I tests/wayland_integration_check.py --ocr-format txt
runtime/python/bin/python3 -I tests/wayland_integration_check.py --ocr-format docx --tap-shift
runtime/python/bin/python3 -I tests/wayland_integration_check.py --ocr-pdf --ocr-format docx
```

A suíte OCR valida imagens, orientação EXIF, PDFs escaneados e mistos, recortes de página, Word editável e originais preservados. A suíte ampliada valida texto Unicode em oito destinos, documentos de cinco tipos, tabelas XLSX, slides PPTX e os oito áudios, incluindo o tom decodificado e os originais. Os testes usam apenas arquivos gerados e temporários: formatos de imagem, transparência, PDFs de várias páginas, extração de texto, união, nomes repetidos, URI com espaços/acentos, erros parciais e cancelamento. O teste Wayland cria seu próprio compositor virtual e barramento D-Bus: não injeta entrada na sessão do usuário. Ele exige que o aplicativo de origem receba `CopyAction`, além de conferir a saída e o hash do original. As prévias e os comprovantes da instalação ficam em `validation/`.

O mesmo teste inicia com `--background` e verifica ausência de janela principal, conversão oculta, chamadas repetidas sem duplicação, reabertura pelo Dolphin/menu, fechamento sem parar o gesto e encerramento explícito com limpeza da integração.

Os testes das ferramentas cobrem compressão, metadados e fotos com arquivos gerados, cópias e cancelamento, além da interface de confirmação. O editor tem verificações de cada ajuste, orientação, transparência, prévias assíncronas, publicação sem sobrescrita e controles por mouse/teclado em janelas de tamanho normal e compacto. Os casos Wayland verificam separadamente as três ações e os diferentes tipos de entrada. Eles usam o compositor privado, exigem `CopyAction`, verificam o hash original e confirmam que o drop abre a janela sem salvar uma cópia sozinho.

Também há uma CLI opcional:

```sh
maracuja --list-formats
maracuja --background
maracuja --convert webp --output /pasta/destino -- /pasta/foto.png
maracuja --convert pdf -- /pasta/foto1.jpg /pasta/foto2.png
maracuja --convert docx -- /pasta/anotacoes.txt
maracuja --convert txt -- /pasta/foto-com-texto.png
maracuja --convert docx -- /pasta/documento-escaneado.pdf
maracuja --convert mp3 -- /pasta/audio.wav
```
