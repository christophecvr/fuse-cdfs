DESCIPTION = "fuse mount audio cd based on libcdio"
MAINTAINER = "christophecvr"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://COPYING;md5=40d2542b8c43a3ec2b7f5da31a697b88"

inherit gitpkgv

PV = "0.01+git${SRCPV}"
PKGV = "0.01+git${GITPKGV}"

SRCREV = "${AUTOREV}"
SRC_URI = "git://github.com/christophecvr/${BPN}.git;branch=testing;protocol=https"

S = "${WORKDIR}/git"

# added to have al m4 macro's into build when using bitbake with -b option.
# Then proceeding to full image build or at least package build with recipes parsing is not needed.
do_configure_prepend() {
	ln -sf ${STAGING_DATADIR_NATIVE}/aclocal/*.m4 ${S}/m4/
}

inherit autotools pkgconfig
