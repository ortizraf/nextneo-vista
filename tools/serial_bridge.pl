#!/usr/bin/perl
# Ponte RFC 2217 (serial por TCP) para o macOS, sem instalar nada: só o Perl do sistema.
# O Docker no macOS não enxerga portas USB; o container conecta em
# rfc2217://host.docker.internal:<porta> e esta ponte repassa para a porta serial real,
# inclusive DTR/RTS (necessários para o esptool colocar a placa em modo de gravação).
#
#   perl tools/serial_bridge.pl /dev/cu.usbmodem1201 <arquivo-onde-gravar-a-porta-tcp>
#
# Escuta só em 127.0.0.1; uma conexão por vez; a serial é aberta a cada conexão.
use strict;
use warnings;
use IO::Socket::INET;
use IO::Select;
use POSIX qw(:termios_h);
use Fcntl;

my ($dev, $portfile) = @ARGV;
die "uso: $0 <porta-serial> <arquivo-porta>\n" unless $dev && $portfile;

# macOS ioctls
use constant { TIOCMBIS => 0x8004746c, TIOCMBIC => 0x8004746b, IOSSIOSPEED => 0x80085402,
               TIOCM_DTR => 0x002, TIOCM_RTS => 0x004 };
use constant { IAC => 255, DONT => 254, DO => 253, WONT => 252, WILL => 251, SB => 250, SE => 240,
               COM_PORT => 44 };
my %SUPPORTED = (0 => 1, 3 => 1, COM_PORT() => 1);   # BINARY, SGA, COM-PORT-OPTION

$SIG{PIPE} = 'IGNORE';
my $srv = IO::Socket::INET->new(LocalAddr => '127.0.0.1', LocalPort => 0, Listen => 1, ReuseAddr => 1)
  or die "ponte: não consegui escutar: $!\n";
open(my $pf, '>', $portfile) or die "ponte: $portfile: $!\n";
print $pf $srv->sockport, "\n";
close $pf;

while (my $c = $srv->accept) { serve($c); close $c; }

sub xioctl { my ($fh, $req, $val) = @_; ioctl($fh, $req, $val) }

sub open_serial {
  my $fh;
  for (1 .. 50) {   # até ~5 s: a placa pode estar reiniciando
    last if sysopen($fh, $dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    undef $fh;
    select(undef, undef, undef, 0.1);
  }
  die "ponte: não abri $dev: $!\n" unless $fh;
  my $t = POSIX::Termios->new;
  $t->getattr(fileno $fh);
  $t->setiflag(0); $t->setoflag(0); $t->setlflag(0);
  $t->setcflag(CS8 | CREAD | CLOCAL | HUPCL);
  $t->setcc(VMIN, 1); $t->setcc(VTIME, 0);
  $t->setattr(fileno $fh, TCSANOW);
  xioctl($fh, IOSSIOSPEED, pack("Q", 115200));
  binmode $fh;
  return $fh;
}

sub write_all {
  my ($fh, $data) = @_;
  my $sel = IO::Select->new($fh);
  while (length $data) {
    my $n = syswrite($fh, $data);
    if (!defined $n) { return 0 unless $!{EAGAIN}; $sel->can_write(1); next; }
    substr($data, 0, $n, '');
  }
  return 1;
}

sub serve {
  my $c = shift;
  binmode $c;
  my $s = eval { open_serial() };
  unless ($s) { warn $@; return; }
  my (%did, %will);
  my ($st, $cmd, $sb) = (0, 0, '');

  my $reply = sub { write_all($c, pack('C*', @_)) };
  my $handle_sb = sub {
    my ($opt, $sub, $data) = (ord(substr($sb, 0, 1)), ord(substr($sb, 1, 1) // "\0"), substr($sb, 2));
    return unless $opt == COM_PORT;
    if ($sub == 1 && length($data) == 4) {                       # SET-BAUDRATE
      my $baud = unpack('N', $data);
      xioctl($s, IOSSIOSPEED, pack('Q', $baud)) if $baud;         # ignorado em USB nativo
    } elsif ($sub == 5 && length $data) {                         # SET-CONTROL
      my $v = ord $data;
      xioctl($s, TIOCMBIS, pack('i', TIOCM_DTR)) if $v == 8;
      xioctl($s, TIOCMBIC, pack('i', TIOCM_DTR)) if $v == 9;
      xioctl($s, TIOCMBIS, pack('i', TIOCM_RTS)) if $v == 11;
      xioctl($s, TIOCMBIC, pack('i', TIOCM_RTS)) if $v == 12;
    } elsif ($sub == 12 && length $data) {                        # PURGE-DATA
      my $v = ord $data;
      POSIX::tcflush(fileno $s, $v == 1 ? TCIFLUSH : $v == 2 ? TCOFLUSH : TCIOFLUSH);
    }
    (my $esc = $data) =~ s/\xff/\xff\xff/g;                       # resposta: mesmo valor
    write_all($c, pack('C*', IAC, SB, COM_PORT, $sub + 100) . $esc . pack('C*', IAC, SE));
  };

  my $sel = IO::Select->new($c, $s);
  LOOP: while (1) {
    for my $h ($sel->can_read) {
      my $n = sysread($h, my $buf, 4096);
      if ($h == $s) {
        next if !defined $n && $!{EAGAIN};
        last LOOP unless $n;                                     # placa desconectou
        $buf =~ s/\xff/\xff\xff/g;
        write_all($c, $buf) or last LOOP;
        next;
      }
      last LOOP unless $n;                                       # cliente fechou
      my $out = '';
      for my $b (unpack 'C*', $buf) {
        if ($st == 0)    { if ($b == IAC) { $st = 1 } else { $out .= chr $b } }
        elsif ($st == 1) {
          if    ($b == IAC)                { $out .= chr 255; $st = 0 }
          elsif ($b == SB)                 { $sb = ''; $st = 3 }
          elsif ($b >= WILL && $b <= DONT) { $cmd = $b; $st = 2 }
          else                             { $st = 0 }
        }
        elsif ($st == 2) {
          if ($cmd == WILL && !$did{$b}) { $did{$b} = 1; $reply->(IAC, $SUPPORTED{$b} ? DO : DONT, $b) }
          if ($cmd == DO && !$will{$b})  { $will{$b} = 1; $reply->(IAC, $SUPPORTED{$b} ? WILL : WONT, $b) }
          $st = 0;
        }
        elsif ($st == 3) { if ($b == IAC) { $st = 4 } else { $sb .= chr $b } }
        elsif ($st == 4) {
          if    ($b == SE)  { write_all($s, $out) or last LOOP; $out = ''; $handle_sb->(); $st = 0 }
          elsif ($b == IAC) { $sb .= chr 255; $st = 3 }
          else              { $st = 3 }
        }
      }
      write_all($s, $out) or last LOOP if length $out;
    }
  }
  close $s;
}
