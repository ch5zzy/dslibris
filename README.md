# dslibris

[![C/C++ CI](https://github.com/ch5zzy/dslibris/actions/workflows/build.yml/badge.svg)](https://github.com/ch5zzy/dslibris/actions/workflows/build.yml)

**dslibris** is an
[EPUB](https://en.wikipedia.org/wiki/EPUB)
ebook reader for the Nintendo DS.

![Browser](etc/sample/browser.png)

![Quickstart](etc/sample/quickstart.png)

![A sample page](etc/sample/iliad.png)

![Dictionary lookup](etc/sample/dictionary.gif)

## Installation

See the
[Quickstart](https://github.com/rhaleblian/dslibris/wiki/User:-Quickstart)
page in the Wiki.

## Development

### Prerequisites

Install devkitPro's toolchain for ARM, aka `devkitARM`.
Then install the required devkitARM packages:

    sudo dkp-pacman -S $(cat pacman.txt)

### Building

Assure that devkitARM is available to your shell:

    . /opt/devkitpro/devkitarm.sh

then

    make

`dslibris.nds` should show up in the top directory.

## See Also

Development Tools: <http://devkitpro.org>

EPUB/W3C/IDPF: [http://idpf.org](https://idpf.org/)
