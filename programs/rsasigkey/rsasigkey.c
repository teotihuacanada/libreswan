/*
 * RSA signature key generation, for libreswan
 *
 * Copyright (C) 1999, 2000, 2001  Henry Spencer.
 * Copyright (C) 2003-2008 Michael C Richardson <mcr@xelerance.com>
 * Copyright (C) 2003-2009 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2009 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012-2015 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2016, Andrew Cagney <cagney@gnu.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>
#include <libreswan.h>
#include "lswalloc.h"
#include "secrets.h"

#include <prerror.h>
#include <prinit.h>
#include <prmem.h>
#include <plstr.h>
#include <key.h>
#include <keyt.h>
#include <nss.h>
#include <pk11pub.h>
#include <seccomon.h>
#include <secerr.h>
#include <secport.h>

#include <time.h>

#include <arpa/nameser.h> /* for NS_MAXDNAME */
#include "constants.h"
#include "lswalloc.h"
#include "lswlog.h"
#include "lswconf.h"
#include "lswnss.h"

#ifdef FIPS_CHECK
#  include <fipscheck.h>
#endif

/*
 * We allow 2192 as a minimum, but default to a random value between 3072 and
 * 4096. The range is used to avoid a mono-culture of key sizes.
 */
#define MIN_KEYBIT 2192

#ifndef DEVICE
# define DEVICE  "/dev/random"
#endif
#ifndef MAXBITS
# define MAXBITS 20000
#endif

#define DEFAULT_SEED_BITS 60 /* 480 bits of random seed */

#define E       3               /* standard public exponent */
/* #define F4	65537 */	/* possible future public exponent, Fermat's 4th number */

char *progname;
char usage[] =
	"rsasigkey [--verbose] [--seeddev <device>] [--configdir <dir>] [--password <password>] [--hostname host] [--seedbits bits] [<keybits>]";
struct option opts[] = {
	{ "rounds",    1,      NULL,   'p', }, /* obsoleted */
	{ "noopt",     0,      NULL,   'n', }, /* obsoleted */

	{ "verbose",   0,      NULL,   'v', },
	{ "seeddev",   1,      NULL,   'S', },
	{ "random",    1,      NULL,   'r', }, /* compat alias for seeddev */
	{ "hostname",  1,      NULL,   'H', },
	{ "help",              0,      NULL,   'h', },
	{ "version",   0,      NULL,   'V', },
	{ "configdir",        1,      NULL,   'c' },
	{ "configdir2",        1,      NULL,   'd' }, /* nss tools use -d */
	{ "password", 1,      NULL,   'P' },
	{ "seedbits", 1,      NULL,   's' },
	{ 0,           0,      NULL,   0, }
};
int verbose = 0;                /* narrate the action? */
char *device = DEVICE;          /* where to get randomness */
int nrounds = 30;               /* rounds of prime checking; 25 is good */
char outputhostname[NS_MAXDNAME];  /* hostname for output */

char me[] = "ipsec rsasigkey";  /* for messages */

/* forwards */
void rsasigkey(int nbits, int seedbits, char *configdir, char *password);
void getrandom(size_t nbytes, unsigned char *buf);
static const char *conv(const unsigned char *bits, size_t nbytes, int format);
void report(char *msg);

/*
 * bundle - bundle e and n into an RFC2537-format chunk_t
 */
static char *base64_bundle(int e, chunk_t modulus)
{
	/*
	 * Pack the single-byte exponent into a byte array.
	 */
	assert(e <= 255);
	u_char exponent_byte = 1;
	chunk_t exponent = {
		.ptr = &exponent_byte,
		.len = 1,
	};

	/*
	 * Create the resource record.
	 */
	char *bundle;
	err_t err = rsa_pubkey_to_base64(exponent, modulus, &bundle);
	if (err) {
		fprintf(stderr, "%s: can't-happen bundle convert error `%s'\n",
			me, err);
		exit(1);
	}

	return bundle;
}

/* UpdateRNG - Updates NSS's PRNG with user generated entropy. */
static void UpdateNSS_RNG(int seedbits)
{
	SECStatus rv;
	int seedbytes = BYTES_FOR_BITS(seedbits);
	unsigned char *buf = alloc_bytes(seedbytes,"TLA seedmix");

	getrandom(seedbytes, buf);
	rv = PK11_RandomUpdate(buf, seedbytes);
	assert(rv == SECSuccess);
	messupn(buf, seedbytes);
	pfree(buf);
}

/*  Returns the password passed in in the text file.
 *  Uses the password once and nulls it out to prevent
 *  PKCS11 from calling us forever.
 */
static char *GetFilePasswd(PK11SlotInfo *slot, PRBool retry, void *arg)
{
	char *phrases, *phrase;
	PRFileDesc *fd;
	PRInt32 nb;
	const char *pwFile = (const char *)arg;
	int i;
	const long maxPwdFileSize = 4096;
	char *tokenName = NULL;
	int tokenLen = 0;

	if (pwFile == NULL)
		return NULL;

	if (retry)
		return NULL;  /* no good retrying - the files contents will be the same */

	phrases = PORT_ZAlloc(maxPwdFileSize);

	if (phrases == NULL)
		return NULL; /* out of memory */

	fd = PR_Open(pwFile, PR_RDONLY, 0);
	if (fd == NULL) {
		fprintf(stderr, "%s: No password file \"%s\" exists.\n", me, pwFile);
		PORT_Free(phrases);
		return NULL;
	}
	nb = PR_Read(fd, phrases, maxPwdFileSize);

	PR_Close(fd);

	if (nb == 0) {
		fprintf(stderr, "%s: password file contains no data\n", me);
		PORT_Free(phrases);
		return NULL;
	}

	if (slot != NULL) {
		tokenName = PK11_GetTokenName(slot);
		if (tokenName != NULL)
			tokenLen = PORT_Strlen(tokenName);
	}
	i = 0;
	do {
		int startphrase = i;
		int phraseLen;
		/* handle the Windows EOL case */
		while (phrases[i] != '\r' && phrases[i] != '\n' && i < nb)
			i++;
		/* terminate passphrase */
		phrases[i++] = '\0';
		/* clean up any EOL before the start of the next passphrase */
		while ( (i < nb) && (phrases[i] == '\r' || phrases[i] == '\n'))
			phrases[i++] = '\0';
		/* now analyze the current passphrase */
		phrase = &phrases[startphrase];
		if (tokenName == NULL)
			break;
		if (PORT_Strncmp(phrase, tokenName, tokenLen))
			continue;
		phraseLen = PORT_Strlen(phrase);
		if (phraseLen < (tokenLen + 1))
			continue;
		if (phrase[tokenLen] != ':')
			continue;
		phrase = &phrase[tokenLen + 1];
		break;
	} while (i < nb);

	phrase = PORT_Strdup((char*)phrase);
	PORT_Free(phrases);
	return phrase;
}

static char *GetModulePassword(PK11SlotInfo *slot, PRBool retry, void *arg)
{
	secuPWData *pwdata = (secuPWData *)arg;
	secuPWData pwnull = { PW_NONE, 0 };
	secuPWData pwxtrn = { PW_EXTERNAL, "external" };
	char *pw;

	if (pwdata == NULL)
		pwdata = &pwnull;

	if (PK11_ProtectedAuthenticationPath(slot))
		pwdata = &pwxtrn;
	if (retry && pwdata->source != PW_NONE) {
		fprintf(stderr, "%s: Incorrect password/PIN entered.\n", me);
		return NULL;
	}

	switch (pwdata->source) {
	case PW_FROMFILE:
		/* Instead of opening and closing the file every time, get the pw
		 * once, then keep it in memory (duh).
		 */
		pw = GetFilePasswd(slot, retry, pwdata->data);
		pwdata->source = PW_PLAINTEXT;
		pwdata->data = strdup(pw);
		/* it's already been dup'ed */
		return pw;

	case PW_PLAINTEXT:
		return strdup(pwdata->data);

	default: /* cases PW_NONE and PW_EXTERNAL not supported */
		fprintf(stderr,
			"%s: Unknown or unsupported case in GetModulePassword\n",
			me);
		break;
	}

	fprintf(stderr, "%s: Password check failed:  No password found.\n",
		me);
	return NULL;
}

/*
   - main - mostly argument parsing
 */
int main(int argc, char *argv[])
{
	const struct lsw_conf_options *oco = lsw_init_options();
	int opt;
	int nbits = 0;
	int seedbits = DEFAULT_SEED_BITS;
	char *configdir = oco->confddir; /* where the NSS databases reside */
	char *password = NULL;  /* password for token authentication */

	while ((opt = getopt_long(argc, argv, "", opts, NULL)) != EOF)
		switch (opt) {
		case 'n':
		case 'p':
			fprintf(stderr, "%s: --noopt and --rounds options have been obsoleted - ignored\n",
				me);
			break;
		case 'v':       /* verbose description */
			verbose = 1;
			break;

		case 'r':
			fprintf(stderr, "%s: Warning: --random is obsoleted for --seeddev. It no longer specifies the random device used for obtaining random key material",
				me);
			/* FALLTHROUGH */
		case 'S':       /* nonstandard random device for seed */
			device = optarg;
			break;

		case 'H':       /* set hostname for output */
			{
				size_t full_len = strlen(optarg);
				bool oflow = sizeof(outputhostname) - 1 < full_len;
				size_t copy_len = oflow ? sizeof(outputhostname) - 1 : full_len;

				memcpy(outputhostname, optarg, copy_len);
				outputhostname[copy_len] = '\0';
			}
			break;
		case 'h':       /* help */
			printf("Usage:\t%s\n", usage);
			exit(0);
			break;
		case 'V':       /* version */
			printf("%s %s\n", me, ipsec_version_code());
			exit(0);
			break;
		case 'c':       /* nss configuration directory */
		case 'd':       /* -d is used for configdir with nss tools */
			configdir = optarg;
			break;
		case 'P':       /* token authentication password */
			password = optarg;
			break;
		case 's': /* seed bits */
			seedbits = atoi(optarg);
			if (PK11_IsFIPS()) {
				if (seedbits < DEFAULT_SEED_BITS) {
					fprintf(stderr, "%s: FIPS mode does not allow < %d seed bits\n",
						me, DEFAULT_SEED_BITS);
					exit(1);
				}
			}
			break;
		case '?':
		default:
			printf("Usage:\t%s\n", usage);
			exit(2);
		}

	if (outputhostname[0] == '\0') {
		if (gethostname(outputhostname, sizeof(outputhostname)) < 0) {
			fprintf(stderr, "%s: gethostname failed (%s)\n",
				me,
				strerror(errno));
			exit(1);
		}
	}

	if (argv[optind] == NULL) {
		/* default: spread bits between 3072 - 4096 in multiple's of 16 */
		srand(time(NULL));
		nbits = 3072 + 16 * (rand() % 64);
	} else {
		unsigned long u;
		err_t ugh = ttoulb(argv[optind], 0, 10, INT_MAX, &u);

		if (ugh != NULL) {
			fprintf(stderr, "%s: keysize specification is malformed: %s\n",
				me, ugh);
			exit(1);
		}
		nbits = u;
	}

	if (nbits < MIN_KEYBIT ) {
		fprintf(stderr, "%s: requested RSA key size of %d is too small - use %d or more\n",
			me, nbits, MIN_KEYBIT);
		exit(1);
	} else if (nbits > MAXBITS) {
		fprintf(stderr, "%s: overlarge bit count (max %d)\n", me,
			MAXBITS);
		exit(1);
	} else if (nbits % (BITS_PER_BYTE * 2) != 0) {
		fprintf(stderr, "%s: bit count (%d) not multiple of %d\n", me,
			nbits, (int)BITS_PER_BYTE * 2);
		exit(1);
	}

	rsasigkey(nbits, seedbits, configdir, password);
	exit(0);
}

/*
 * generate an RSA signature key
 *
 * e is fixed at 3, without discussion.  That would not be wise if these
 * keys were to be used for encryption, but for signatures there are some
 * real speed advantages.
 * See also: https://www.imperialviolet.org/2012/03/16/rsae.html
 */
void rsasigkey(int nbits, int seedbits, char *configdir, char *password)
{
	SECStatus rv;
	PK11RSAGenParams rsaparams = { nbits, (long) E };
	secuPWData pwdata = { PW_NONE, NULL };
	PK11SlotInfo *slot = NULL;
	SECKEYPrivateKey *privkey = NULL;
	SECKEYPublicKey *pubkey = NULL;
	realtime_t now = realnow();

	if (password == NULL) {
		pwdata.source = PW_NONE;
	} else {
		/* check if passwd == configdir/nsspassword */
		size_t cdl = strlen(configdir);
		size_t pwl = strlen(password);
		static const char suf[] = "/nsspassword";

		if (pwl == cdl + sizeof(suf) - 1 &&
			memeq(password, configdir, cdl) &&
			memeq(password + cdl, suf, sizeof(suf)))
			pwdata.source = PW_FROMFILE;
		else
			pwdata.source = PW_PLAINTEXT;
	}
	pwdata.data = password;

	lsw_nss_buf_t err;
	if (!lsw_nss_setup(configdir, FALSE/*rw*/, GetModulePassword, err)) {
		fprintf(stderr, "%s: %s\n", me, err);
		exit(1);
	}

#ifdef FIPS_CHECK
	if (PK11_IsFIPS() && !FIPSCHECK_verify(NULL, NULL)) {
		fprintf(stderr,
			"FIPS HMAC integrity verification test failed.\n");
		exit(1);
	}
#endif

	if (PK11_IsFIPS() && password == NULL) {
		fprintf(stderr,
			"%s: On FIPS mode a password is required\n",
			me);
		exit(1);
	}

	/* Good for now but someone may want to use a hardware token */
	slot = PK11_GetInternalKeySlot();
	/* In which case this may be better */
	/* slot = PK11_GetBestSlot(CKM_RSA_PKCS_KEY_PAIR_GEN, password ? &pwdata : NULL); */
	/* or the user may specify the name of a token. */

#if 0
	if (PK11_IsFIPS() || !PK11_IsInternal(slot)) {
		rv = PK11_Authenticate(slot, PR_FALSE, &pwdata);
		if (rv != SECSuccess) {
			fprintf(stderr, "%s: could not authenticate to token '%s'\n",
				me, PK11_GetTokenName(slot));
			return;
		}
	}
#endif /* 0 */

	/* Do some random-number initialization. */
	UpdateNSS_RNG(seedbits);
	/* Log in to the token */
	if (password != NULL) {
		rv = PK11_Authenticate(slot, PR_FALSE, &pwdata);
		if (rv != SECSuccess) {
			fprintf(stderr,
				"%s: could not authenticate to token '%s'\n",
				me, PK11_GetTokenName(slot));
			return;
		}
	}
	privkey = PK11_GenerateKeyPair(slot,
				       CKM_RSA_PKCS_KEY_PAIR_GEN,
				       &rsaparams, &pubkey,
				       PR_TRUE,
				       password != NULL? PR_TRUE : PR_FALSE,
				       &pwdata);
	/* inTheToken, isSensitive, passwordCallbackFunction */
	if (privkey == NULL) {
		fprintf(stderr,
			"%s: key pair generation failed: \"%d\"\n", me,
			PORT_GetError());
		return;
	}

	chunk_t public_modulus = {
		.ptr = pubkey->u.rsa.modulus.data,
		.len = pubkey->u.rsa.modulus.len,
	};
	chunk_t public_exponent = {
		.ptr = pubkey->u.rsa.publicExponent.data,
		.len = pubkey->u.rsa.publicExponent.len,
	};

	char *hex_ckaid;
	{
		SECItem *ckaid = PK11_GetLowLevelKeyIDForPrivateKey(privkey);
		if (ckaid == NULL) {
			fprintf(stderr, "%s: 'CKAID' calculation failed\n", me);
			exit(1);
		}
		hex_ckaid = strdup(conv(ckaid->data, ckaid->len, 16));
		SECITEM_FreeItem(ckaid, PR_TRUE);
	}

	/*privkey->wincx = &pwdata;*/
	PORT_Assert(pubkey != NULL);
	fprintf(stderr, "Generated RSA key pair with CKAID %s was stored in the NSS database\n",
		hex_ckaid);

	/* and the output */
	report("output...\n");  /* deliberate extra newline */
	printf("\t# RSA %d bits   %s   %s", nbits, outputhostname,
		ctime(&now.real_secs));
	/* ctime provides \n */
	printf("\t# for signatures only, UNSAFE FOR ENCRYPTION\n");

	printf("\t#ckaid=%s\n", hex_ckaid);

	/* RFC2537/RFC3110-ish format */
	{
		char *bundle = base64_bundle(E, public_modulus);
		printf("\t#pubkey=%s\n", bundle);
		pfree(bundle);
	}

	printf("\tModulus: 0x%s\n", conv(public_modulus.ptr, public_modulus.len, 16));
	printf("\tPublicExponent: 0x%s\n", conv(public_exponent.ptr, public_exponent.len, 16));

	if (hex_ckaid != NULL)
		free(hex_ckaid);
	if (privkey != NULL)
		SECKEY_DestroyPrivateKey(privkey);
	if (pubkey != NULL)
		SECKEY_DestroyPublicKey(pubkey);

	lsw_nss_shutdown(LSW_NSS_CLEANUP);
}

/*
 * getrandom - get some random bytes from /dev/random (or wherever)
 * NOTE: This is only used for additional seeding of the NSS RNG
 */
void getrandom(size_t nbytes, unsigned char *buf)
{
	size_t ndone;
	int dev;
	ssize_t got;

	dev = open(device, 0);
	if (dev < 0) {
		fprintf(stderr, "%s: could not open %s (%s)\n", me,
			device, strerror(errno));
		exit(1);
	}

	ndone = 0;
	if (verbose) {
		fprintf(stderr, "getting %d random seed bytes for NSS from %s...\n",
			(int) nbytes * BITS_PER_BYTE,
			device);
	}
	while (ndone < nbytes) {
		got = read(dev, buf + ndone, nbytes - ndone);
		if (got < 0) {
			fprintf(stderr, "%s: read error on %s (%s)\n", me,
				device, strerror(errno));
			exit(1);
		}
		if (got == 0) {
			fprintf(stderr, "%s: eof on %s!?!\n", me, device);
			exit(1);
		}
		ndone += got;
	}

	close(dev);
}

/*
   - conv - convert bits to output in specified datatot format
 * NOTE: result points into a STATIC buffer
 */
static const char *conv(const unsigned char *bits, size_t nbytes, int format)
{
	static char convbuf[MAXBITS / 4 + 50];  /* enough for hex */
	size_t n;

	n = datatot(bits, nbytes, format, convbuf, sizeof(convbuf));
	if (n == 0) {
		fprintf(stderr, "%s: can't-happen convert error\n", me);
		exit(1);
	}
	if (n > sizeof(convbuf)) {
		fprintf(stderr,
			"%s: can't-happen convert overflow (need %d)\n",
			me, (int) n);
		exit(1);
	}
	return convbuf;
}

/*
   - report - report progress, if indicated
 */
void report(msg)
char *msg;
{
	if (!verbose)
		return;

	fprintf(stderr, "%s\n", msg);
}
