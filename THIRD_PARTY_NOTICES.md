# Componentes de terceiros do Maracujá

O código próprio do Maracujá está sob a licença MIT, reproduzida em
[`LICENSE`](LICENSE). Os programas, bibliotecas, modelos e fontes distribuídos
com o aplicativo continuam sujeitos às respectivas licenças e avisos de autoria.
A licença MIT do Maracujá não substitui esses termos.

Este documento orienta a leitura dos avisos incluídos no pacote. A relação exata
de arquivos e versões depende da plataforma e da compilação: consulte
`licenses/shipped-files.json`, os arquivos de versões travadas em `licenses/` e
os avisos originais indicados abaixo. Preserve esses arquivos ao redistribuir.

## Interface e integração

**Qt e seus componentes de terceiros.** A interface utiliza Qt Core, GUI, QML,
Quick, Quick Controls, Widgets, Network e Concurrent; a integração Linux também
utiliza Qt D-Bus. Bibliotecas e plugins adicionais podem ser incluídos conforme
o ambiente de compilação. Os módulos Qt utilizados oferecem licenças livres
LGPL/GPL conforme seus próprios avisos; módulos opcionais possuem condições
diferentes. Não há uma licença única que substitua as licenças dos componentes
de terceiros incorporados no Qt.

- Linux: avisos dos pacotes em `licenses/Ubuntu/*.copyright` e textos completos
  referenciados em `licenses/common/`.
- Windows: textos completos e avisos originais da tag oficial Qt 6.8.3 em
  `licenses/Qt/6.8.3/`, organizados por módulo. `sources.json` registra as
  origens e os hashes de cada documento; a coleta é obrigatória no empacotador.
- Linux: quando disponíveis na distribuição de origem, os arquivos
  `licenses/Qt-SBOM/qtbase-*.spdx` e `qtdeclarative-*.spdx` preservam os SBOMs
  dos módulos de origem. Eles também descrevem componentes de compilação e
  partes não distribuídas; não substituem o inventário final do Maracujá.

Referências oficiais: [licenças Qt 6.8](https://doc.qt.io/qt-6.8/licensing.html),
[licenças Qt 6.11](https://doc.qt.io/qt-6.11/licensing.html),
[LGPL no Qt](https://doc.qt.io/qt-6.11/lgpl.html) e
[componentes de terceiros do Qt](https://doc.qt.io/qt-6.11/licenses-used-in-qt.html).

As bibliotecas Qt são distribuídas separadamente do executável. O pacote
portátil pode ser extraído para substituir bibliotecas por versões compatíveis;
um AppImage também permite extração. O código e as instruções de compilação do
Maracujá estão no repositório do projeto.

**Integração Linux e bibliotecas nativas.** Cairo, Pango, GLib/GIO, Wayland,
bibliotecas de imagem, codecs e demais dependências nativas conservam os avisos
dos pacotes de origem em `licenses/Ubuntu/`. O backend de som libcanberra é
opcional e pode ser fornecido pelo sistema. Drivers gráficos, glibc e o
carregador do sistema não são copiados pelo empacotador Linux.

## Conversão, OCR e edição

| Componente | Licenças e avisos incluídos |
| --- | --- |
| Python | Licença PSF e avisos de componentes incorporados. No Linux, veja `runtime/python/lib/python3.12/LICENSE.txt`; no Windows, `runtime/python/Lib/LICENSE.txt`, conforme o runtime distribuído. As receitas e versões do runtime estão em `licenses/runtime.lock.json` e nas referências de fontes. |
| Pacotes Python | Cada pacote mantém seus arquivos `*.dist-info/licenses/`, `LICENSE*`, `NOTICE*` e demais avisos em `runtime/python/`. As versões e hashes ficam em `licenses/python-packages.lock.json` e `python-requirements.lock`. |
| PDFium e pypdfium2 | Licenças BSD, Apache e avisos específicos dos componentes distribuídos, preservados nos diretórios de licenças do pacote `pypdfium2` dentro do runtime Python. |
| LibreOffice | MPL 2.0 e outras licenças dos componentes incorporados, conforme `runtime/libreoffice/LICENSE`, `NOTICE` e demais avisos mantidos na árvore do LibreOffice. |
| FFmpeg | A licença depende da configuração e das bibliotecas incorporadas. O pacote Windows travado usa a variante MSYS2 GPL-3.0-or-later; o pacote Linux conserva os avisos da distribuição que forneceu os binários. Veja `licenses/MSYS2/` ou `licenses/Ubuntu/` e os respectivos inventários de versões. |
| Poppler e dados de caracteres | Poppler usa GPL-2.0-or-later; dados e dependências possuem avisos próprios. Veja `licenses/MSYS2/`, `licenses/msys2-packages.lock.json` no Windows, ou os avisos e versões em `licenses/Ubuntu/` no Linux. |
| Tesseract e modelos OCR | Tesseract e os modelos `tessdata_fast` utilizados têm licença Apache 2.0. A biblioteca preserva o aviso do pacote de origem; os modelos e seus hashes estão em `ocr/tessdata/manifest.json`, com a licença em `ocr/tessdata/LICENSE`. |

Referências oficiais: [licença do Python](https://docs.python.org/3.12/license.html),
[licenças do LibreOffice](https://www.libreoffice.org/licenses/),
[licença do FFmpeg](https://ffmpeg.org/legal.html),
[Poppler](https://poppler.freedesktop.org/),
[Tesseract](https://github.com/tesseract-ocr/tesseract) e
[modelos tessdata_fast](https://github.com/tesseract-ocr/tessdata_fast).

## Fontes tipográficas e ferramentas de distribuição

Quando incluídas, as fontes DejaVu mantêm a licença Bitstream Vera e os avisos
das alterações DejaVu; as fontes Liberation mantêm a SIL Open Font License 1.1.
Os avisos dos pacotes Linux são copiados para `licenses/Ubuntu/`. As fontes do
sistema operacional que não acompanham o pacote não passam a integrar sua
distribuição. Referências:
[DejaVu](https://dejavu-fonts.github.io/License.html) e
[Liberation](https://github.com/liberationfonts/liberation-fonts/blob/main/LICENSE).

Os instaladores Windows são gerados com
[Inno Setup](https://jrsoftware.org/files/is/license.txt), de Jordan Russell e
colaboradores. A embalagem AppImage utiliza o runtime do projeto
[AppImage](https://github.com/AppImage/type2-runtime/blob/main/LICENSE), que
também incorpora bibliotecas com licenças próprias. Os avisos dessas ferramentas
não são substituídos pela licença MIT do aplicativo.

Bibliotecas de execução de compiladores distribuídas no Windows mantêm seus
próprios termos, incluindo as exceções aplicáveis ao runtime GCC/MinGW quando
utilizado, com textos completos em `licenses/MinGW/`. Os avisos das dependências MSYS2 estão em `licenses/MSYS2/`; isso não
atribui a mesma licença a todos os arquivos do pacote Windows.

## Inventário e obtenção das fontes

- `licenses/shipped-files.json` identifica os arquivos do pacote por caminho
  relativo, tamanho e SHA-256.
- `licenses/SOURCES.json` reúne referências aos projetos e fontes upstream.
- `licenses/runtime.lock.json` e os arquivos `*.lock*` registram versões,
  origens e hashes dos runtimes e dependências travadas.
- Linux: `licenses/ubuntu-packages.txt` e
  `licenses/distribution-sources.json` registram versões dos pacotes binários e
  de seus pacotes-fonte. A recuperação deve utilizar a versão e o repositório
  correspondentes à distribuição que produziu o binário.
- Windows: `licenses/msys2-packages.lock.json` registra os pacotes de origem,
  suas licenças declaradas e referências aos pacotes-fonte MSYS2.

Uma referência upstream não demonstra, por si só, que aquele arquivo contém
todas as alterações de um pacote da distribuição. Quando houver patches de
Ubuntu, KDE neon ou MSYS2, também são necessários os materiais correspondentes
dessa versão. Os links e inventários acima são referências técnicas; sua
presença não equivale a uma confirmação de que todas as fontes correspondentes
já foram obtidas, preservadas ou disponibilizadas junto de um release.

Ao preparar uma redistribuição, mantenha os avisos completos e disponibilize as
fontes e demais materiais exigidos pelas licenças dos componentes efetivamente
distribuídos. Este resumo não reduz nem modifica os direitos concedidos por
essas licenças.
