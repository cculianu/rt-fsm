#!/usr/bin/perl -w

use strict;
use English;
use Cwd;

my @supportedFedoras = (8); # TODO add support for fedora 9 too!
my @supportedArches = ("i686", "x86_64");
my $fedoraRelease = undef; # set to one of @supportedFedoras below..
my $arch = undef; # after script sanity checks, gets set to one of @supportedArches below..
my %archRTAIPatches = ( 
                        i686 => 'base/arch/i386/patches/hal-linux-2.6.23-i386-1.12-00.patch',
                        x86_64 => 'base/arch/x86_64/patches/hal-linux-2.6.23-x86_64-1.4-00.patch'
                      );
my %archKernelConfigs = (
                          i686 => 'utils/config-i686-fc8',
                          x86_64 => 'utils/config-x86_64-fc8'
                        );
my %archRTAIConfigs = (
                          i686 => 'utils/rtai_config-i686-fc8',
                          x86_64 => 'utils/rtai_config-x86_64-fc8'
                        );
my %archbzImage = ( i686 => 'arch/i386/boot/bzImage',
                    x86_64 => 'arch/x86_64/boot/bzImage' );
my $workdir = '/tmp/rtfsm-install-dir';
my $kerneldir = undef;
my $kernelver = undef;
my $rtaidir = undef;
my $comedidir = undef;
my $comedilibdir = undef;
my $isDevel = (`hostname | tr -d '[[:space:]]'` eq 'localhost.localdomain') ? 1 : undef;
my $stage = undef;

# ANSI COLORS
my $CRE="\n" . '[K';
my $NORMAL='[0m';
# RED: Failure or error message
my $RED='[1;31m';
# GREEN: Success message
my $GREEN='[1;32m';
# YELLOW: Descriptions
my $YELLOW='[1;33m';
# BLUE: System messages
my $BLUE='[1;34m';
# MAGENTA: Found devices or drivers
my $MAGENTA='[1;35m';
# CYAN: Questions
my $CYAN='[1;36m';
# BOLD WHITE: Hint
my $WHITE='[1;37m';
my $BLINK='[5m';
my $UNDERSCORE='[4m';
my $WHITEONRED='[1;37;41m';
my $WHITEONBLUE='[1;37;44m';
my $WHITEONMAGENTA='[1;37;45m';

sub centerTxt
{
    my $t = shift or die "centerTxt() needs at least 1 parameter!";
    my $tstripped = $t;
    $tstripped =~ s/[[:cntrl:]][[][^mK]+[mK]//g;
    $tstripped =~ s/$//g;
    my $linelen = shift || 80; # assume 80 char linelen if not specified
    my $n = int ( ($linelen - length($tstripped))/2 );
    for (my $i = 0; $i < $n; ++$i) {
            $t = " ${t} "; # grow string with spaces before and after
    }
    return $t;
}

sub printBanner()
{
    print $BLUE 
        . centerTxt("------------------------------------------------------------------------------") . "\n"
        . $WHITE 
        . centerTxt("${WHITE}rtfsm installer${NORMAL}") . "\n"
        . centerTxt("(for Fedora-based systems)${NORMAL}") . "\n"
        . centerTxt("$GREEN by Calin Culianu ${RED}<${YELLOW}calin\@ajvar.org${RED}>$NORMAL") . "\n"
        . $BLUE
        . centerTxt("------------------------------------------------------------------------------") . "\n"
        . $NORMAL;
}

sub chkIsRoot()
{
    die "This script requires root (UID 0) privileges -- please re-run as root!\n" unless ($EUID == 0);
}

sub chkOS()
{
    open FH, "</etc/issue" or die "Cannot open /etc/issue to determine distro\n";
    defined(my $issueline = <FH>) or die "Cannot read /etc/issue\n";
    close FH;
    $issueline =~ m/Fedora release (\d+)/i or die "You don't appear to be running Fedora, sorry!\n";
    $fedoraRelease = $1;
    foreach my $r (@supportedFedoras) {
# first make sure the fedora release is found in our list of supported fedoras
        if ($r == $fedoraRelease) {
# next, make sure the architecture supported
            foreach my $a (@supportedArches) {
                    $arch = `uname -p`;
                    chomp $arch;
                    return 1 if ($arch eq $a);
            }
            die "You appear to be a supported Fedora, but not on architecture " . (join " or ", @supportedArches) . ".\n";
        }
    }
    
    die "Sorry, you need to be running Fedora " . (join " or ", @supportedFedoras) . ".\n";
    return undef; # not reached
}

sub chkHaveGcc()
{
    system("gcc --version > /dev/null && make --version > /dev/null") and die "Error -- cannot find gcc!\n\nGcc is ${RED}required${NORMAL} to compile the kernel and the rtfsm!\n";
    return 1;
}

sub chkIsInSrcDir()
{
    my @stuff2check = ( 'Makefile', 'kernel', 'runtime', 'user', 'include' );
    foreach my $f (@stuff2check) {
        -e $f or die "This script needs to be run from the top-level source directory of rtfsm.  Please change to that directory and re-run.\n";
    }
    return 1;
}

sub setupWorkDir()
{
    if (!$isDevel) {
        system("rm -fr '" . $workdir ."'");
        mkdir(${workdir}) or die "Cannot create $workdir\n"; 
    } else { # devel!
        mkdir(${workdir}); 
    }
    return 1;
}

sub downloadKernel()
{
    my $kernelURL = "http://www.kernel.org/pub/linux/kernel/v2.6/linux-2.6.23.tar.bz2";
    my $tarball = `basename "$kernelURL"`;
    chomp $tarball;
    $kerneldir = "/usr/src/".`basename "$tarball" .tar.bz2`;
    chomp $kerneldir;
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Downloading Vanilla Linux Kernel\n\n$NORMAL";
    system("cd '" . ${workdir} . "' && wget -c --no-check-certificate '" . ${kernelURL} . "'") and die "Cannot download $kernelURL\n";
    print "\n$WHITEONBLUE=======>$NORMAL$RED       Deleting all of $MAGENTA$kerneldir\n\n$NORMAL";
    system("rm -fr $kerneldir");
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Untarring $MAGENTA$tarball$YELLOW to $MAGENTA$kerneldir\n\n$NORMAL";
    system("cd '/usr/src' && tar -xvjf '$workdir/" . $tarball . "'") and die "Failed to untar the file $tarball\n";
    system("ln -sf '$kerneldir' /usr/src/linux");
    return 1;
}

sub downloadRTAI()
{
    my $url = "https://www.rtai.org/RTAI/rtai-3.6.tar.bz2";
    my $tarball = `basename "$url"`;
    chomp $tarball;
    $rtaidir = "/usr/src/".`basename "$tarball" .tar.bz2`;
    chomp $rtaidir;
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Downloading RTAI 3.6\n\n$NORMAL";
    system("cd '" . ${workdir} . "' && wget -c --no-check-certificate '" . ${url} . "'") and die "Cannot download $url\n";
    print "\n$WHITEONBLUE=======>$NORMAL$RED       Deleting all of $MAGENTA$rtaidir\n\n$NORMAL";
    system("rm -fr $rtaidir");
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Untarring $MAGENTA$tarball$YELLOW to $MAGENTA$rtaidir\n\n$NORMAL";
    system("cd '/usr/src' && tar -xvjf '$workdir/" . $tarball . "'") and die "Failed to untar the file $tarball\n";
    system("ln -sf '$rtaidir' /usr/src/rtai");
    return 1;
}

sub downloadComedi()
{
    my $url1 = "http://www.comedi.org/download/comedi-0.7.76.tar.gz";
    my $url2 = "http://www.comedi.org/download/comedilib-0.8.1.tar.gz";
    my $tarball1 = `basename "$url1"`;
    chomp $tarball1;
    my $tarball2 = `basename "$url2"`;
    chomp $tarball2;
    $comedidir = "/usr/src/" . `basename "$tarball1" .tar.gz`;
    chomp $comedidir;
    $comedilibdir = "/usr/src/" . `basename "$tarball2" .tar.gz`;
    chomp $comedilibdir;
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Downloading COMEDI Drivers\n\n$NORMAL";
    system("cd '" . ${workdir} . "' && wget -c --no-check-certificate '" . ${url1} . "'") and die "Cannot download $url1\n";
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Downloading COMEDI Library\n\n$NORMAL";
    system("cd '" . ${workdir} . "' && wget -c --no-check-certificate '" . ${url2} . "'") and die "Cannot download $url2\n";
    print "\n$WHITEONBLUE=======>$NORMAL$RED       Deleting all of $MAGENTA$comedidir$RED and $MAGENTA$comedilibdir\n\n$NORMAL";
    system("rm -fr '$comedidir' '$comedilibdir'");
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Untarring $MAGENTA$tarball1$YELLOW to $MAGENTA$comedidir\n\n$NORMAL";
    system("cd '/usr/src' && tar -xvzf '$workdir/" . $tarball1 . "'") and die "Failed to untar the file $tarball1\n";
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Untarring $MAGENTA$tarball2$YELLOW to $MAGENTA$comedilibdir\n\n$NORMAL";
    system("cd '/usr/src' && tar -xvzf '$workdir/" . $tarball2 . "'") and die "Failed to untar the file $tarball2\n";
    system("ln -sf '$comedidir' /usr/src/comedi");
    system("ln -sf '$comedilibdir' /usr/src/comedilib");
    return 1;
}

sub askUserIsDelOk()
{
    print "\n${NORMAL}This script is about to download a bunch of files (linux kernel, rtai, COMEDI)\n"
          ."and put them in /usr/src, possibly blowing away existing files!\n"
          ."\nIt will also compile a new kernel and install it, possibly rendering your\nsystem unbootable!\n"
          ."\n${RED}Are you SURE you want to continue${NORMAL} [y/N] ? $NORMAL";
    my $reply = <STDIN>;
    chomp $reply;
    die "Ok, aborted.\n" unless $reply =~ /^[yY]/;
}

sub patchKernel()
{
    my $olddir = getcwd();
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Patching Kernel$NORMAL\n\n";
    chdir($kerneldir);
    (my $patchfile = $archRTAIPatches{$arch}) or die "Unknown arch '$arch' $archRTAIPatches{$arch}";
    system("patch -p1 < $rtaidir/$patchfile") and die "Patch failed.\n";
    chdir($olddir);
    return 1;
}

sub compileKernel()
{
    my $f = $archKernelConfigs{$arch};
    $kernelver = `grep CONFIG_LOCALVERSION= $f | cut -f 2 -d '='  | cut -f 2 -d '"'`;
    chomp $kernelver;
    $kerneldir =~ m/linux-(.*)/;
    $kernelver = "$1$kernelver";
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Compiling Kernel in $MAGENTA$kerneldir$NORMAL\n\n";
    system("cp -f '$f' '$kerneldir' && make -C '$kerneldir' oldconfig") and die "Unable to setup .config for kernel\n";
    system("cd '$kerneldir' && make -j2 && make -j2 modules") and die "Unable to compile kernel\n";
    return 1;
}

sub genGrubEntry($)
{
    my @lines = (split("\n", shift));
    my $ret = "";
    foreach my $line (@lines) {
        my @words = split /\s+/, $line;
        while ( (scalar @words) && $words[0] =~ /^\s*$/ ) {
            shift @words; # get rid of leading empty tokens?
        }
        next unless (scalar @words); # skip blank lines..
        if ($words[0] eq 'title') {
# replace title..
           $ret .= "title Fedora w/ kernel $kernelver (RTAI) for rtfsm\n";
        } elsif ($words[0] =~ /^kernel/) { # special handling for kernel grub conf directive
            shift @words; # pop off leading 'kernel'
            # parse kernel path..
            my @toks = split "/", $words[0];
            $toks[$#toks] = "bzImage-$kernelver";
            $words[0] = join "/", @toks;
            $ret .= "\tkernel " . (join " ", @words) . "\n";
        } elsif ($words[0] =~ /^initrd/) { # special handling for initrd grub conf directive
            shift @words; # pop off leading 'initrd'
            # parse initrd path..
            my @toks = split "/", $words[0];
            $toks[$#toks] = "initrd-$kernelver.img";
            $words[0] = join "/", @toks;
            $ret .= "\tinitrd " . (join " ", @words) . "\n";
        } else { # otherwise just blindly concat..
            $ret .= "\t" . (join " ",@words) . "\n";
        }
    }
    return $ret;
}

sub editGrubConf()
{
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Editing $MAGENTA/boot/grub/grub.conf$YELLOW to boot $MAGENTA/boot/bzImage-$kernelver$NORMAL\n\n";
    open FH, "</boot/grub/grub.conf" or die "Cannot open /boot/grub/grub.conf for reading!\n";
    my ($line,$needtitle,$intitle,$titlepos,$conf,$sampleentry) = (undef,1,0,undef,"","");
    while ($line = <FH>) {
        my @words = split /\s+/, $line;
        if (@words && $words[0] eq 'title') {
            if ($needtitle && !$intitle) {
                $intitle = 1;
                $needtitle = 0;
                $titlepos = length($conf);
            } elsif(!$needtitle) {
                $intitle = 0;
            }
        }
        if ($intitle) {
# read a sample entry
            $sampleentry .= $line;
        }
#overwrite default= line.. with default=0, always
        if ($line =~ /^default=\d+/) {
            $line="default=0\n";
        }
        $conf .= $line;
    }
    close FH;
    (my $newentry = genGrubEntry($sampleentry)) || die "Error generating new grub entry!"; 
    $conf = substr($conf, 0, $titlepos) . $newentry . substr($conf, $titlepos);
    open FH, ">/boot/grub/grub.conf" or die "Cannot open /boot/grub/grub.conf for writing!\n";
    print FH $conf;
    close FH;
    return 1;
}

sub installKernel()
{
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Installing Modules to $MAGENTA/lib/modules/$kernelver$NORMAL\n\n";
    system("cd '$kerneldir' && make modules_install") and die "Error installing kernel modules\n";
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Installing Kernel to $MAGENTA/boot/bzImage-$kernelver$NORMAL\n\n";
    my $bz = $archbzImage{$arch};
    system("cd '$kerneldir' && cp -fv $bz /boot/bzImage-$kernelver && cp -fv .config /boot/config-$kernelver && cp -fv System.map /boot/System.map-$kernelver") and die "Error installing the kernel\n";
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Generating $MAGENTA/boot/initrd-$kernelver.img$NORMAL\n\n";
    system("mkinitrd -v /boot/initrd-$kernelver.img $kernelver") and die "\n${RED}Error making the initrd ramdisk image!${NORMAL}\n";
    print ("\nTo boot the kernel, you need to modify grub.conf.  You can allow this script to\ndo it for you, or you can do it manually if you have a custom setup.\n\n");
    print("${RED}Allow this script to modify /boot/grub/grub.conf ${WHITE}[y/N]$NORMAL ? ");
    if (<STDIN> =~ /^[yY]/) {
        editGrubConf();
    } else {
        print("${CYAN}Ok, manually edit grub.conf and hit enter when done!$NORMAL ");
        <STDIN>;
    }
    return 1;
}

sub getStage()
{
    open FH, "<$workdir/STAGE" or return 1; # no file, we are in stage 1, definitely
    defined(my $tmp = <FH>) or die "Could not read $workdir/STAGE\n";
    close FH;
    chomp $tmp;
    $tmp =~ /(\d+)/ or die "Parse error reading $workdir/STAGE\n";
    return $1
}

sub setStage($)
{
    my $newstage = shift;
    if (!defined($newstage)) {
        `rm -f '$workdir/STAGE'`; # forcibly reset the stage..
        return 1;
    }
    open FH, ">$workdir/STAGE" or die "Cannot open $workdir/STAGE for writing!\n";
    print FH "$newstage\n" or die "Cannot append to STAGE file!\n";
    close FH;
    return 1;
}

sub reboot()
{
    system("init 6") and die "Could not reboot system! ARGH!\n";
    return undef; # not reached
}

sub compileRTAI()
{
    $rtaidir = "/usr/src/rtai" if (!defined($rtaidir));
    my $cf = $archRTAIConfigs{$arch};
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Compiling RTAI in $MAGENTA$rtaidir$NORMAL\n\n";
    system("make -C '$rtaidir' distclean") and die "Failed to make distclean in $rtaidir\n";
    system("cp -fv $cf '$rtaidir'/.rtai_config && make -C '$rtaidir' oldconfig") and die "Failed to make oldconfig in $rtaidir\n";
    system("make -C '$rtaidir'") and die "Failed to compiled RTAI in $rtaidir\n";
    return 1;
}

sub installRTAI($)
{
    my $doDepmod = shift;
    $rtaidir = "/usr/src/rtai" if (!defined($rtaidir));
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Installing RTAI$NORMAL\n\n";
    system("make -C '$rtaidir' install") and die "Failed to install RTAI in $rtaidir\n";
    system("mkdir -p /lib/modules/`uname -r`/rtai && ln -sf /usr/realtime/modules/* /lib/modules/`uname -r`/rtai/") and die "Failed to symlink RTAI modules.\n";
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Running depmod$NORMAL\n\n";
    system("depmod -ae") if ($doDepmod);
    if (system('grep -q /usr/realtime/lib /etc/ld.so.conf') != 0) {
        system('echo /usr/realtime/lib >> /etc/ld.so.conf');
    }
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Running ldconfig$NORMAL\n\n";
    system('ldconfig');
    return 1;
}

sub compileComedi()
{
    $comedidir = "/usr/src/comedi" if (!defined($comedidir));
    $comedilibdir = "/usr/src/comedilib" if (!defined($comedilibdir));
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Compiling COMEDI in $MAGENTA$comedidir$NORMAL\n\n";
    system("cd '$comedidir' && ./configure --with-rtaidir=/usr/realtime --with-linuxdir=/usr/src/linux") and die "Failed to configure COMEDI in $comedidir\n";
    system("cd '$comedidir' && make -j2") and die "Failed to compile COMEDI in $comedidir\n";
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Compiling COMEDILIB in $MAGENTA$comedilibdir$NORMAL\n\n";
    system("cd '$comedilibdir' && ./configure") and die "Failed to configure COMEDILIB in $comedilibdir\n";
    system("cd '$comedilibdir' && make -j2") and die "Failed to compile COMEDILIB in $comedilibdir\n";
    return 1;
}

sub installComedi()
{
    $comedidir = "/usr/src/comedi" if (!defined($comedidir));
    $comedilibdir = "/usr/src/comedilib" if (!defined($comedilibdir));
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Installing COMEDI from $MAGENTA$comedidir$NORMAL to kernel module dir\n\n";
    system("cd '$comedidir' && make install") and die "Failed to install COMEDI from $comedidir\n";
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Running depmod$NORMAL\n\n";
    system('depmod -ae');
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Installing COMEDILIB from $MAGENTA$comedilibdir$NORMAL  to /usr/local\n\n";
    system("cd '$comedilibdir' && make install") and die "Failed to install COMEDILIB from $comedilibdir\n";
    if (system('grep -q /usr/local/lib /etc/ld.so.conf') != 0) {
        system('echo /usr/local/lib >> /etc/ld.so.conf');
    }
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Running ldconfig$NORMAL\n\n";
    system('ldconfig');
    return 1;
}

sub compileRTFSM()
{
    print "\n$WHITEONBLUE=======>$NORMAL$YELLOW       Compiling RTFSM!$NORMAL\n\n";
    system("make") and die "\n\n${RED}${BLINK}Failed to compile${NORMAL}\n\nSomething is amiss!  Check the INSTALL doc or email Calin!\n\n";
    return 1;
}


sub complainAboutFirewall()
{
    if (system('/etc/init.d/iptables status > /dev/null') == 0) { # iptables firewall is running
        print "
A firewall (iptables) is running on this system.  It is recommended you 
disable it (despite possible security concerns) otherwise you won't be able 
to talk to the FSMServer or SoundServer via the network.\n\n";
        print "${RED}Allow this script to disable the firewall ${WHITE}[y/N]$NORMAL ? ";
        if (<STDIN> =~ /^[yY]/) {
            system("/etc/init.d/iptables stop") and die ("Unable to disable the iptables firewall\n");
            system("chkconfig --level 2345 iptables off") and die ("Unable to disable the iptables firewall service\n");
        }
    }
    return 1;
}

sub doneMsg()
{
    print "\n\n${GREEN}Installation (mostly) done!$NORMAL\n\n${YELLOW}Here's what's been accomplished:${NORMAL}\n"
         ."   - A new RTAI-patched kernel was compiled and installed and booted into\n"
         ."   - ${CYAN}rtai${NORMAL}, ${CYAN}comedi${NORMAL}, and ${CYAN}comedilib$NORMAL have all been compiled and installed\n"
         ."   - ${CYAN}rtfsm$NORMAL (this code) has been compiled\n\n"
         ."\n${RED}What remains to be done:$NORMAL\n"
         ."   - You need to run ${MAGENTA}./load_modules.sh${NORMAL} to start the appropriate kernel modules\n"
         ."   - If you don't have a National Instruments board, you need to manually\n"
         ."     configure COMEDI (otherwise load_modules.sh will do it for you)\n"
         ."   - Start ${MAGENTA}./FSMServer$NORMAL and optionally $MAGENTA./SoundServer$NORMAL\n\n";
   return 1;
}

sub main()
{
    printBanner();
    chkHaveGcc();
    chkIsRoot();
    chkOS();
    chkIsInSrcDir();

    $stage = getStage();
    
    if ($stage == 1) {
# STAGE 1, delete all working dirs, download all needed software, recompile kernel
        askUserIsDelOk();
        setupWorkDir();
        downloadRTAI();
        downloadComedi();
        downloadKernel();
        patchKernel();
        compileKernel();
        installKernel();
        setStage(2);
        
        print  "\n\n${NORMAL}Ok, new kernel is installed -- now we need to reboot.\n"
              ."Once reboot is done, ${BLINK}${WHITEONRED}re-run this script from the same directory$NORMAL to continue.\n"
              ."${CYAN}Press enter to reboot..${NORMAL} ";
        <STDIN>;
        
        reboot();
        
    } elsif ($stage == 2) {
# STAGE 2, compile and install rtai, comedi; compile rtfsm
        compileRTAI();
        installRTAI(0);
        compileComedi();
        installComedi();
        compileRTFSM();
        complainAboutFirewall();
        setStage(undef);
        system("rm -fr '$workdir'");
        doneMsg();
        return 0;
        
    } else {
        die "Unknown installer stage";
    }
    return 0;
}


exit main();

