# Integração do Maracujá com o KDE

O Maracujá é um aplicativo nativo para converter imagens, PDFs, documentos e áudios locais. A janela principal usa Qt Quick, o núcleo de conversão usa C++/Qt e as rodas de arraste usam pequenos processos Wayland nativos, com renderização Cairo/Pango. A versão 0.9.0 acrescentou compressão e inspeção/edição de metadados em uma segunda roda, por padrão com **Alt + arrastar**. A roda de conversão mantém **Shift + arrastar** como padrão. Os dois atalhos são configuráveis; a conversão também permite abertura direta, sem arraste. O reconhecimento local de texto e a inicialização em segundo plano continuam disponíveis. Na versão 0.9.1, soltar em Comprimir ou Metadados abre uma janela flutuante independente, arredondada, próxima ao cursor, sem exibir nem restaurar a janela principal. Fechar as opções mantém o aplicativo residente e os gestos ativos; operações em andamento têm cancelamento explícito. A versão 0.9.2 reduz as janelas para 440 px de largura, recolhe metas de compressão em Mais opções e dados técnicos em Detalhes, e simplifica a visualização de formatos somente leitura. A versão 0.9.3 compartilha a pasta escolhida no painel com compressão e metadados; sem destino personalizado, os resultados continuam ao lado dos originais. A versão 0.9.4 substitui o botão Ferramentas pelo seletor **Conversão / Edição** acima da roda. A seleção de arquivos é compartilhada, mas formatos genéricos só podem ser adicionados em Edição; esses arquivos não habilitam conversões. Clicar em Comprimir ou Metadados abre as opções independentes sem gravar. Enter só inicia conversão no respectivo modo, e os gestos de conversão sempre abrem a roda de formatos.

## Uso

Abra **Maracujá** pelo menu de aplicativos e adicione arquivos pelo seletor ou arrastando-os para a janela. Escolha o formato entre as opções oferecidas para a seleção. A disponibilidade depende do tipo de entrada e dos codificadores detectados; uma associação MIME não garante todos os pares de conversão.

Com o aplicativo em execução no **KDE Plasma Wayland**, deixe o controle **Ativo** ligado; **Atalho** permite escolher outro gesto. Comece a arrastar um arquivo compatível, mova o ponteiro e **depois toque Shift**. A roda aparece sob o cursor quando recebe um arraste válido: leve o arquivo até um formato e solte. Os formatos mudam conforme a entrada. É possível soltar Shift antes de escolher ou mantê-lo pressionado. Soltar no centro ou sair da roda cancela. A janela principal pode permanecer escondida em segundo plano.

Para acessar as ferramentas, comece a arrastar um ou mais arquivos locais, mova o ponteiro e **depois toque Alt**. A segunda roda oferece **Comprimir** e **Metadados**. Soltar em uma dessas opções abre o painel correspondente; a operação só começa após a confirmação no painel. O botão **Ferramentas** permite abrir o mesmo painel pela janela. Na configuração **Atalho**, as abas **Conversão** e **Ferramentas** guardam combinações independentes; ferramentas sempre exige arraste e continua disponível quando a conversão usa **Somente teclas**. O aplicativo recusa dois atalhos iguais.

As ferramentas aceitam até 128 arquivos regulares legíveis, sem incluir diretórios. A compressão automática otimiza JPEG, PNG, WebP e PDF; outros tipos são agrupados em ZIP. A opção ZIP aceita toda a seleção e mantém nomes únicos dentro do arquivo. Os perfis Equilibrado/Forte, dimensões máximas e metas de tamanho/redução ajustam a tentativa; a meta não é garantida e o resultado mostra os tamanhos reais. JPEG, WebP e imagens de PDF podem perder qualidade. Um arquivo já compactado pode não diminuir.

Metadados editáveis estão disponíveis em JPEG, PNG, WebP, TIFF, PDF, DOCX, XLSX e PPTX. O painel permite pesquisar grupos e campos, editar valores suportados e marcar sua remoção. Formato, dimensões e outros campos técnicos ficam somente leitura; orientação e perfis de cor necessários à aparência são preservados. Outros tipos oferecem informações básicas em modo de leitura. **Salvar cópia** cria um arquivo novo. A opção de remover todos atua nos campos suportados de metadados, sem remover conteúdo visível, comentários, anexos ou revisões do documento.

`--background` inicia o aplicativo sem apresentar a janela principal. No modo residente, o ícone na bandeja oferece **Abrir Maracujá** e **Sair**. Fechar a janela pelo **X** apenas a esconde; o gesto continua disponível e uma conversão em andamento não é cancelada. **Sair** encerra o aplicativo e sua integração depois de cancelar cooperativamente uma conversão em andamento. Abrir novamente pelo menu de aplicativos ou pelo Dolphin mostra a instância existente. Uma segunda execução de `--background` sem arquivos não abre a janela nem cria outra instância.

No Dolphin, selecione imagens, PDFs, documentos ou áudios locais, clique com o botão direito e escolha **Converter com Maracujá…**, eventualmente dentro de **Ações**. A ação envia a seleção diretamente à roda compacta com `--wheel %U`. Ela permanece disponível mesmo sem usar o gesto. O serviço anuncia os tipos MIME registrados no arquivo `.desktop`, e o aplicativo valida cada arquivo antes de oferecer formatos. Se o Maracujá já estiver aberto, sua instância existente recebe a seleção por D-Bus. Separe imagens, PDFs, documentos e áudio em seleções diferentes.

As conversões criam novos arquivos, normalmente ao lado dos originais. Nomes existentes recebem um sufixo; os originais não são sobrescritos.

Também é possível abrir arquivos pelo terminal:

```sh
~/.local/bin/maracuja "/caminho/foto com espaços.png"
~/.local/bin/maracuja --wheel "/caminho/documento.pdf"
~/.local/bin/maracuja --wheel "/caminho/anotações.txt"
~/.local/bin/maracuja --wheel "/caminho/gravação.wav"
~/.local/bin/maracuja --background
```

A identidade visual usa um maracujá e uma roda violeta com detalhes dourados. A abertura e a transição do realce entre os formatos são animadas.

## Arquivos da integração

| Arquivo no projeto | Destino de instalação por usuário |
| --- | --- |
| `integration/org.winux.Mandarina.desktop` | `~/.local/share/applications/org.winux.Mandarina.desktop` |
| `integration/org.winux.Mandarina-autostart.desktop`, apenas com `--enable-autostart` | `~/.config/autostart/org.winux.Mandarina.desktop` |
| `integration/mandarina-convert.desktop` | `~/.local/share/kio/servicemenus/mandarina-convert.desktop` |
| `assets/org.winux.Mandarina.svg` | `~/.local/share/icons/hicolor/scalable/apps/org.winux.Maracuja.svg`, com cópia de compatibilidade no nome anterior |
| `build/maracuja` | `~/.local/lib/mandarina/maracuja` |
| `build/maracuja-gesture-helper` | `~/.local/lib/mandarina/maracuja-gesture-helper` |
| Lançador criado por `install-user.py` | `~/.local/bin/maracuja` e `~/.local/bin/mandarina` (compatibilidade) |
| `integration/kwin/mandarina-shift/` | `~/.local/share/kwin/effects/mandarina-shift/` |
| `integration/kwin/window-hints.js` | `~/.local/share/kwin/effects/mandarina-shift/window-hints.js` |

A versão 0.9.5 padroniza os nomes visíveis, o ícone e os executáveis como **Maracujá** (`maracuja` e `maracuja.exe`). Os nomes técnicos dos arquivos, as classes `org.winux.Mandarina`, o serviço D-Bus e o efeito `mandarina-shift` permanecem compatíveis com a instalação anterior, preservando os lançadores fixados e a entrada de autoinicialização.

O aplicativo registra tipos de imagem/PDF, texto simples, Markdown, HTML, DOCX, ODT, RTF e áudio compatível como entradas. Ele passa a aparecer entre as opções de **Abrir com** para esses arquivos; instalar este lançador não deve substituir as associações padrão do usuário. Uma entrada MIME torna a ação visível, mas não promete qualquer par de conversão: a roda depende das ferramentas e dos codificadores locais disponíveis. EPUB é oferecido como saída, não como entrada. Apple Pages não é aceito como entrada nem destino nesta versão.

O seletor agrupa imagens, documentos/PDF e áudio. Na lista, imagens recebem uma miniatura, documentos mostram a extensão e áudio recebe um símbolo musical. Os ajustes de qualidade só aparecem para saídas de imagem; a resolução em DPI aparece ao converter páginas de PDF ou documentos para PNG/JPG. Não são apresentados controles de qualidade de áudio ou de OCR. Rodas com mais formatos usam rótulos compactos, e o centro mostra a descrição do formato sob o ponteiro.

Documentos de texto simples, Markdown, HTML, DOCX, ODT e RTF podem gerar PDF, DOCX, ODT, RTF, TXT, Markdown, HTML, EPUB ou páginas em PNG/JPG. Eles podem perder detalhes de formatação entre formatos. Conversões de PDF para TXT/DOCX/Markdown/HTML/EPUB preservam a extração de texto existente e usam OCR local nas páginas sem texto selecionável. O texto é reorganizado, sem reconstrução garantida da diagramação. PDF → PPTX usa uma imagem por página/slide, sem tornar os elementos editáveis. PDF → XLSX extrai somente tabelas detectadas em PDFs com texto; se não houver tabela, retorna um erro claro. Não há reconstrução OCR de planilhas. Os resultados de extração de tabelas precisam ser conferidos. Áudio com perdas não recupera qualidade por ser salvo em outro formato.

Imagens oferecem **TXT** e **DOCX** na mesma roda, pela janela, pelo Dolphin e pelo gesto **Arrastar + Shift**. Esses destinos reconhecem o texto em português e inglês pelo Tesseract local (`por+eng`); no Word, o resultado é texto editável reorganizado. O reconhecimento pode errar acentos, caracteres, colunas e ordem de leitura. Recomenda-se revisar o resultado. Nenhuma imagem ou página é enviada à nuvem. Em PDFs mistos, a decisão de usar OCR é feita por página: o texto selecionável existente não é substituído por reconhecimento de imagem.

O campo `Exec` dos lançadores de arquivos usa `%U` para receber múltiplas URLs como argumentos separados. O instalador aplica as regras de aspas e escapes de Desktop Entry ao caminho absoluto do executável, incluindo diretórios pessoais com espaços. Os caminhos recebidos não são executados como comandos. A ação do Dolphin restringe o protocolo a `file`; o aplicativo também recusa URLs remotas e interpreta URLs de arquivo por `QUrl::toLocalFile()`. Nenhum arquivo é enviado à internet.

`install-user.py` instala o aplicativo em `~/.local`, marca o serviço do Dolphin como executável e guarda cópias dos arquivos anteriores em `validation/install-<data>/`. A presença dos modelos no projeto não instala a integração. O instalador não muda associações padrão.

Somente com `--enable-autostart`, o instalador cria ou atualiza a entrada própria em `~/.config/autostart/org.winux.Mandarina.desktop`, com `Exec` absoluto seguido de `--background`, `TryExec`, `StartupNotify=false` e `OnlyShowIn=KDE;`. A mesma rotina de cópia de segurança e instalação atômica protege a entrada anterior. Sem essa opção, uma entrada existente é preservada. Nenhuma entrada de outro aplicativo é modificada. O recibo informa se a entrada foi criada e se a autoinicialização foi alterada nesta instalação. A autoinicialização pode ser desativada nas configurações de início automático do KDE; **Sair** pela bandeja encerra somente a execução atual.

## Arquitetura do gesto no Wayland

Uma janela Wayland comum não escolhe livremente sua posição global nem recebe dados de um arraste que está em outra superfície. A implementação usa a integração do KWin para posicionar exclusivamente o alvo próprio do Maracujá:

1. `DesktopIntegration` carrega temporariamente o efeito `mandarina-shift` e o script `mandarina-window-hints`, e inicia duas instâncias próprias de `maracuja-gesture-helper`: conversão e ferramentas (`--tools`). Cada uma tem sua própria superfície Wayland, ofertas de arraste e tokens.
2. No modo com arraste, o efeito observa posição do ponteiro, botão esquerdo e a transição exata dos modificadores configurados após um deslocamento mínimo. Combinações com tecla final são registradas no KGlobalAccel com ações temporárias da sessão; conflitos são recusados e os atalhos anteriores são preservados. No modo somente teclas da conversão, a aplicação ouve apenas a ação registrada pelo próprio efeito e abre sua roda compacta sem mover o probe. Durante a configuração, a integração fica suspensa para a captura ser local ao campo focado. Esse é apenas um candidato a gesto; o efeito não recebe nomes nem conteúdo dos arquivos. Ele identifica os alvos pela classe `org.winux.Mandarina` e pelos títulos reservados `MaracujaShiftProbe` e `MaracujaToolsProbe`. Ao mudar de roda no mesmo arraste, minimiza somente o outro alvo próprio.
3. O script remove a decoração do alvo, o mantém acima das janelas e fora da barra/alternador, e procura preservar o foco anterior. A notificação de restauração do alvo arma um timeout curto para candidatos sem arraste válido. Essas propriedades não são gravadas nas configurações do KDE.
4. O alvo Wayland recebe `text/uri-list` apenas quando um arraste entra em sua própria superfície. Ele aceita arquivos locais legíveis e pede ao processo Qt as opções compatíveis por JSON em pipes privados. Os tokens identificam a prévia atual e o campo `mode` distingue `convert` de `tools`; as respostas voltam ao processo de origem. A roda só é desenhada após essa validação. As ferramentas usam somente os identificadores `tool:compress` e `tool:metadata`.
5. Cada helper configura diretamente `wl_data_offer.set_actions(COPY, COPY)`: a máscara do destino permite exclusivamente cópia, inclusive com Shift mantido. Só finaliza drops com cópia negociada. Conversões seguem para o núcleo Qt; ferramentas abrem seu painel e aguardam confirmação. O processamento acontece fora da thread da interface, e os resultados continuam acessíveis na janela principal. Conversão e ferramentas compartilham a condição de ocupado para impedir trabalhos concorrentes e novos drops durante uma operação.

O alvo nativo foi separado porque os backends de arraste Qt/GDK avaliados podem anunciar mais ações do que a preferência de cópia indicada pela interface. Para preservar os originais, a roda externa restringe a máscara diretamente no protocolo, em vez de apenas indicar preferência por cópia.

A integração funciona enquanto o Maracujá está em execução e a opção está ativada, inclusive em segundo plano sem a janela principal. Ao desativá-la, o efeito é descarregado e os alvos ficam indisponíveis. Ao encerrar normalmente o aplicativo, ele também encerra seus helpers e descarrega o script. A residência é do próprio aplicativo na sessão do usuário; não há serviço de sistema instalado. A entrada de autoinicialização é opcional, conforme descrito acima. Somente as combinações configuradas com tecla final são registradas como atalhos globais; não há captura de texto digitado nem leitura de clipboard/histórico. As configurações do Deskflow não são alteradas.

O gesto depende dessas APIs do KWin e não é uma integração genérica para todos os compositores Wayland. A janela normal e o menu do Dolphin continuam sendo formas independentes de abrir arquivos. `--no-gesture` inicia o programa sem carregar a integração global.

## Desenvolvimento e validação

As dependências e os alvos são definidos pelo `CMakeLists.txt`: Qt6 e as bibliotecas de desenvolvimento Wayland, GLib, Cairo e Pango. PDF e áudio também usam os utilitários locais Poppler e FFmpeg/FFprobe. OCR usa Tesseract e modelos locais de português e inglês. As conversões de documentos, slides e tabelas dependem de um runtime privado com LibreOffice e Python; o arquivo `runtime-paths.json` registra os caminhos para a instalação do aplicativo. Cada conversão LibreOffice usa um perfil temporário próprio para não utilizar nem modificar o perfil pessoal do usuário. Na raiz do repositório, execute builds e testes sequencialmente:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 1
python3 install-user.py --no-build
# Opcional: iniciar em segundo plano nos próximos logins do KDE.
python3 install-user.py --no-build --enable-autostart
```

Validação leve dos arquivos de integração:

```sh
desktop-file-validate integration/org.winux.Mandarina.desktop
desktop-file-validate integration/org.winux.Mandarina-autostart.desktop
desktop-file-validate integration/mandarina-convert.desktop
```

O lançador segue a especificação Desktop Entry. O serviço usa a extensão KDE `Type=Service`, com `X-KDE-ServiceTypes=KonqPopupMenu/Plugin`, `MimeType` e `Actions`. A versão do `desktop-file-validate` deste sistema pode apontar que `MimeType` e `Actions` pertencem a `Type=Application`; essa limitação do validador genérico também ocorre em serviços instalados pelo KDE e não deve ser contornada mudando o serviço para `Type=Application`. Confirme adicionalmente que a ação aparece no Dolphin e envia corretamente seleções com espaços, acentos e múltiplos arquivos.

Os testes de arraste usam somente arquivos gerados e um compositor virtual com barramento D-Bus e diretórios XDG privados. O injetor recusa sockets que não tenham o prefixo `mandarina-test-` e a marca de isolamento. Não envia eventos à sessão real do usuário. Execute os casos separadamente, em sequência:

```sh
python3 tests/wayland_integration_check.py
python3 tests/wayland_integration_check.py --tap-shift
python3 tests/wayland_tools_check.py --tool compress --input-kind image
python3 tests/wayland_tools_check.py --tool metadata --input-kind image
python3 tests/wayland_tools_check.py --tool compress --input-kind binary
ctest --test-dir build --output-on-failure -j1
```

A primeira mantém Shift até o drop; a segunda o solta antes de escolher o setor. Ambas exigem que a aplicação de origem receba `CopyAction`, que exista uma saída convertida e que o SHA256 do original continue igual. Hash preservado, isoladamente, não substitui a verificação da ação recebida pela origem. Esses comandos descrevem o procedimento de validação; os comprovantes de cada execução ficam separados dos arquivos de integração.

Os três casos de ferramentas verificam a segunda roda com Alt: imagem para compressão, imagem para metadados e arquivo binário para ZIP. Confirmam foco e propriedades das janelas próprias, negociação de cópia, hash original preservado e abertura do painel correto sem iniciar uma operação no drop. Os testes CTest incluem o motor de ferramentas e a interface QML, com dados gerados. Execute as checagens sequencialmente; os testes não interagem com os arquivos pessoais nem com a sessão gráfica do usuário.

Em QML, `DragEvent.keys` identifica os dados do arrasto, não as teclas pressionadas. Testes sintéticos de eventos Qt não substituem a negociação real de ações no protocolo Wayland.

## Referências técnicas

- [KDE: criar menus de serviço do Dolphin](https://develop.kde.org/docs/apps/dolphin/service-menus/)
- [Freedesktop: especificação Desktop Entry](https://specifications.freedesktop.org/desktop-entry-spec/latest/)
- [Qt: DropArea](https://doc.qt.io/qt-6/qml-qtquick-droparea.html) e [DragEvent](https://doc.qt.io/qt-6/qml-qtquick-dragevent.html)
- [Qt: particularidades de janelas no Wayland](https://doc.qt.io/qt-6/application-windows.html)
- [Wayland: negociação de ações de um destino de arraste](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_data_offer)
- [Qt 6.11.1: negociação de ações no backend Wayland](https://github.com/qt/qtbase/blob/v6.11.1/src/plugins/platforms/wayland/qwaylanddatadevice.cpp)
- [KWin 6.8: efeito e eventos de ponteiro/modificadores](https://github.com/KDE/kwin/blob/Plasma/6.8/src/effect/effecthandler.cpp)
