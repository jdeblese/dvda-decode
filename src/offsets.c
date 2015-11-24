#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
/*#ifdef HAVE_SYS_PARAM_H
#   include <sys/param.h>
#endif
#ifdef HAVE_PWD_H
#   include <pwd.h>
#endif*/
#include <errno.h>
#include <fcntl.h>

#include <unistd.h>

/*#ifdef HAVE_LIMITS_H
#   include <limits.h>
#endif

#ifdef HAVE_DIRECT_H
#   include <direct.h>
#endif*/

#include <dirent.h>

#include "dvdcpxm.h"

#include "dvdread/dvd_reader.h"
#include "dvdread/dvd_udf.h"


#define _MAX_PATH 4096

int main(int argc, char* argv[])
{
	DIR *mntpt;
	struct dirent *indir;
	char dvddevice[_MAX_PATH];
	char mountpath[_MAX_PATH];
	char audiopath[_MAX_PATH];
	char outpath[_MAX_PATH];
	char encrypted[_MAX_PATH];
	char decrypted[_MAX_PATH];

	dvdcpxm_t dvdcpxm;
	dvd_reader_t *r_dvd;
	uint32_t offset, filesize;
	char tmp[_MAX_PATH];
	uint8_t count;

	int outfd = 0;

	if (argc < 3)
	{
		printf("Use: %s <dvd_device> <mount_path> [offsets file]\n", argv[0]);
		return 1;
	}

	if (argc == 4)
	{
		outfd = open(argv[3], O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
		count = 0;
		write(outfd, &count, 1);
	}

	mntpt = opendir(argv[2]);
	if ( !mntpt ) {
		perror("opendir");
		return -1;
	}
	while ( (indir = readdir(mntpt)) ) {
		if ( !strcmp(indir->d_name, "AUDIO_TS") )
			break;
	}
	if ( !indir ) {
		printf("Error: No AUDIO_TS folder found, check if media is mounted\n");
		return -1;
	}
	closedir(mntpt);
	strcpy(mountpath, argv[2]);

	/* Open the device and get the various keys needed */
	strcpy(dvddevice, argv[1]);
	if ( !(r_dvd = DVDOpen(dvddevice)) ) {
		fprintf(stderr, "offsets: Unable to open %s\n", dvddevice);
		return -1;
	}

	/* Open the AUDIO_TS folder */
	strcpy(audiopath, mountpath);
	if ( audiopath[strlen(audiopath) - 1] != '/' )
		strcat(audiopath, "/");
	strcat(audiopath, "AUDIO_TS/");
	mntpt = opendir(audiopath);
	if ( !mntpt ) {
		perror("opendir");
		return -1;
	}

	/* Process all relevant files */
	while ( (indir = readdir(mntpt)) ) {
		if ( indir->d_name[0] != '.' ) {
			strcpy(tmp, "/AUDIO_TS/");
			strcat(tmp, indir->d_name);
			offset = UDFFindFile(r_dvd, tmp, &filesize);
			printf("%8.8x %8.8x %s\n", offset, filesize/DVD_VIDEO_LB_LEN, tmp);
			if (outfd)
			{
				uint8_t len = strlen(indir->d_name);
				uint32_t sectors = filesize/DVD_VIDEO_LB_LEN;
				write(outfd, &len, 1);
				write(outfd, indir->d_name, len);
				write(outfd, &offset, 4);
				write(outfd, &sectors, 4);
			}
			count++;
		}
	}
	closedir(mntpt);

	/*
	// Open the VIDEO_TS folder
	strcpy(audiopath, mountpath);
	if ( audiopath[strlen(audiopath) - 1] != '/' )
		strcat(audiopath, "/");
	strcat(audiopath, "VIDEO_TS/");
	mntpt = opendir(audiopath);
	if ( !mntpt ) {
		perror("opendir");
		return -1;
	}

	// Process all relevant files
	while ( (indir = readdir(mntpt)) ) {
		if ( indir->d_name[0] != '.' ) {
			strcpy(tmp, "/VIDEO_TS/");
			strcat(tmp, indir->d_name);
			offset = UDFFindFile(r_dvd, tmp, &filesize);
			printf("%8.8x %8.8x %s\n", offset, filesize/2048, tmp);
		}
	}
	closedir(mntpt);
	*/

	if (outfd)
	{
		lseek(outfd, 0, SEEK_SET);
		write(outfd, &count, 1);
		close(outfd);
	}

	DVDClose(r_dvd);
	return 0;
}
