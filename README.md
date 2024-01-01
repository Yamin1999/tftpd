TFTP Server
===========

Simple TFTP server implementation defined by RFC1350.

Implemented in pure C, no dependencies.

Usage
-----

The usage is really simple:
```
usage:
	./gcc -o tftpd tftpd.c
	./tftpd
```

Supported concurrent processing of read requests from clients, even for the same file, enhancing efficiency.
