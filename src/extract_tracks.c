#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>

#include "defs.h"

#include "dvdread/dvd_reader.h"
#include "dvdread/dvd_udf.h"

#define _MAX_PATH 4096

#define WIDTH 64
//#define HEIGHT 64

#pragma pack(1)
/* Taken from http://dvd.sourceforge.net/dvdinfo/ifo.html */

#define OUTFILEFMT "title-%02d-%02d."
#define OUTFILELEN 16  // Including \0

struct sectormap
{
	char *name;
	uint32_t offset, sectors;
	struct sectormap *next, *prev;
};

struct sectormap * read_sector_map(char *filename)
{
	int fd = open(filename, O_RDONLY);
	if (!fd)
	{
		perror("open");
		return 0;
	}

	uint8_t count, i, len;
	char *name;
	struct sectormap *ptr = 0, *new;

	read(fd, &count, 1);
	for (i = 0; i < count; i++)
	{
		read(fd, &len, 1);
		name = malloc(len + 1);
		read(fd, name, len);
		name[len] = 0;
		//if (!strncmp(name + len - 3, "AOB", 3))
		//{
			new = malloc(sizeof(struct sectormap));
			memset(new, 0, sizeof(struct sectormap));
			new->name = name;
			read(fd, &new->offset, sizeof(new->offset));
			read(fd, &new->sectors, sizeof(new->sectors));
			if (!ptr)
				ptr = new;
			else {
				while (new->offset < ptr->offset && ptr->prev)
					ptr = ptr->prev;
				while (new->offset > ptr->offset && ptr->next)
					ptr = ptr->next;

				// new is now <= ptr->offset, or ptr is at a boundary

				if (new->offset > ptr->offset && !ptr->next)
				{
					ptr->next = new;
					new->prev = ptr;
				}
				else if (!ptr->prev)
				{
					ptr->prev = new;
					new->next = ptr;
				}
				else
				{
					ptr->prev->next = new;
					new->prev = ptr->prev;
					new->next = ptr;
					ptr->prev = new;
				}
			}
		/*}
		else
		{
			free(name);
			lseek(fd, 8, SEEK_CUR);
		}*/
	}
	
	// Rewind
	if (ptr)
		while (ptr->prev)
			ptr = ptr->prev;
	
	close(fd);

	return ptr;
}

void free_sector_map(struct sectormap *ptr)
{
	while(ptr->prev)
		ptr = ptr->prev;
	while(ptr->next)
	{
		free(ptr->name);
		ptr = ptr->next;
		free(ptr->prev);
	}
	free(ptr->name);
	free(ptr);
}

struct pts_time
{
	uint32_t pts;
	int hour, min, sec, msec, ticks;
};

struct ifo_header
{
	uint32_t set_end_sector;
	uint32_t ifo_end_sector;
	uint16_t version;
	uint32_t vts_category;
	uint32_t atsi_mat_end_sector;
	uint32_t vob_start_sector;
	uint32_t pgci_sector;
};

struct ifo_title
{
	uint16_t flags;
	uint32_t format;
};

struct ifo_tbl2_trackinfo
{
	uint8_t trackno;
	uint16_t flags;
	struct pts_time start_time, duration;
};

struct ifo_tbl3_trackloc
{
	uint32_t signature, start, stop;
};

struct ifo_cell
{
	uint32_t flags;
	uint32_t offset;

	uint8_t ntracks;

	struct pts_time total_length;

	uint16_t table_offsets[4];

	struct ifo_tbl2_trackinfo *tbl2;
	struct ifo_tbl3_trackloc *tbl3;
};

void pts2struct(uint8_t *data, struct pts_time *pts)
{
	uint32_t time;

	memcpy(&pts->pts, data, 4);
	B2N_32(pts->pts);
	time = pts->pts;

	pts->ticks = time % 90;  time /= 90;  // 90 kHz clock
	pts->msec = time % 1000; time /= 1000;
	pts->sec = time % 60;    time /= 60;
	pts->min = time % 60;    time /= 60;
	pts->hour = time % 24;   time /= 24;

	if (time)
		fprintf(stderr, "Warning: PTS time exceeds 24 hours!");
}

void check_padding(uint8_t *data, int count)
{
        int j;
        for ( j = 0; j < count; j++)
                if ( *(data + j) )
                        break;
        if ( j < count )
        {
                printf("\nNonzero Padding\n");
        }
}

/** Read IFO header
 * 
 * data - pointer to head of IFO
 */
int read_ifo_header(uint8_t *data, struct ifo_header *hdr)
{
	uint32_t four;
	if ( memcmp(data, "DVDAUDIO-ATS", 12) )
		return 0;

	memcpy(&hdr->set_end_sector,      data+0x0c, 4); B2N_32(hdr->set_end_sector);
	check_padding(data + 0x10, 12); // Fix this
	memcpy(&hdr->ifo_end_sector,      data+0x1c, 4); B2N_32(hdr->ifo_end_sector);
	memcpy(&hdr->version,             data+0x20, 2); B2N_16(hdr->version);
	memcpy(&hdr->vts_category,        data+0x22, 4); B2N_32(hdr->vts_category);
	check_padding(data + 0x26, 90); // Fix this
	memcpy(&hdr->atsi_mat_end_sector, data+0x80, 4); B2N_32(hdr->atsi_mat_end_sector);
	check_padding(data + 0x84, 60); // Fix this

	memcpy(&four, data+0xc0, 4); B2N_32(four);
	fprintf(stderr, "Warning: Unknown value %8.8x\n", four);

	memcpy(&hdr->vob_start_sector,    data+0xc4, 4); B2N_32(hdr->vob_start_sector);

	memcpy(&four, data+0xc8, 4); B2N_32(four);
	fprintf(stderr, "Warning: Unknown value %8.8x\n", four);

	memcpy(&hdr->pgci_sector,         data+0xcc, 4); B2N_32(hdr->pgci_sector);
	check_padding(data + 0xd0, 48); // Fix this

	return 0x100;
}

/** Read title list
 *
 * ptr - pointer to start of title list
 */
int read_ifo_title(uint8_t *ptr, struct ifo_title *title)
{
	int i;

	memcpy(title, ptr, 6);
	B2N_16(title->flags);
	B2N_32(title->format);

	// FIXME
	fprintf(stderr, "Warning: unknown bytes");
	for (i = 6; i < 16; i++)
		fprintf(stderr, " %2.2x", *(ptr+i));
	fprintf(stderr, "\n");

	return 6;
}

int read_ifo_cell(uint8_t *ptr, uint8_t *sectorptr, struct ifo_cell *ifocell)
{
	int j;
	uint8_t *tblptr, *row;
	uint8_t tmp;

	memset(ifocell, 0, sizeof(struct ifo_cell));

	memcpy(&ifocell->flags,  ptr    , 4); B2N_32(ifocell->flags);
	memcpy(&ifocell->offset, ptr + 4, 4); B2N_32(ifocell->offset);
	tblptr = sectorptr + ifocell->offset;

	check_padding(tblptr, 2);

	memcpy(&ifocell->ntracks, tblptr+2, 1);
	memcpy(&tmp,              tblptr+3, 1);
	if (ifocell->ntracks != tmp)
		fprintf(stderr, "Warning: mismatch in table number of tracks (%d vs %d)\n", ifocell->ntracks, tmp);

	pts2struct(tblptr+4, &ifocell->total_length);

	memcpy(ifocell->table_offsets, tblptr+8, 8);
	for (tmp = 0; tmp < 4; tmp++)
		B2N_16(ifocell->table_offsets[tmp]);


	if (ifocell->table_offsets[1])  // Table 2: track flags and timing
	{
		ifocell->tbl2 = (struct ifo_tbl2_trackinfo *)malloc(ifocell->ntracks * sizeof(struct ifo_tbl2_trackinfo));
		for (j = 0; j < ifocell->ntracks; j++)
		{
			row = tblptr + ifocell->table_offsets[1] + j*0x14;  // Entries are 0x14 bytes long

			// <flags, 2> <0x00> <0x00> <trackno> <0x00> <start, 4> <duration, 4> <0x00,6>
			memcpy(&ifocell->tbl2[j].trackno, row + 4, 1);
			memcpy(&ifocell->tbl2[j].flags,   row    , 2); B2N_16(ifocell->tbl2[j].flags);
			pts2struct(row + 6, &ifocell->tbl2[j].start_time);
			pts2struct(row + 10, &ifocell->tbl2[j].duration);

			check_padding(row + 2, 2);
			check_padding(row + 5, 1);
			check_padding(row + 0xe, 6);

		}
	}

	if (ifocell->table_offsets[2])  // Table 3: track sector offsets
	{
		ifocell->tbl3 = (struct ifo_tbl3_trackloc *)malloc(ifocell->ntracks * sizeof(struct ifo_tbl3_trackloc));
		for (j = 0; j < ifocell->ntracks; j++)
		{
			row = tblptr + ifocell->table_offsets[2] + j*0x0c;
			memcpy(&ifocell->tbl3[j], row, 12);
			if (ifocell->tbl3[j].signature == 0x00000001)
			{
				B2N_32(ifocell->tbl3[j].start);
				B2N_32(ifocell->tbl3[j].stop);
			}
		}
	} else printf("                         Table 3 not present\n");
	return 0;
}

char *joinpath(char *dir, char *file)
{
	char *path = malloc(strlen(dir) + strlen(file) + 2);
	if (!path)
	{
		perror("malloc");
		exit(-1);
	}
	int len;
	strcpy(path, dir);
	len = strlen(path);
	if (path[len-1] != '/')
	{
		path[len] = '/';
		path[len + 1] = 0;
	}
	strcat(path, file);
	return path;
}

int main(int argc, char **argv)
{
	int fd, i, j, count;
	uint8_t buf[WIDTH];
	dvd_reader_t *r_dvd;
	uint8_t *p_blocks, *table, *ptr, oneblock[2048];
	char atsfile[_MAX_PATH] = "AUDIO_TS/ATS_01_0.IFO";

	if ( argc != 5 ) {
		printf( "Usage: %s <sector map file> <ifo file> <aob directory> <output directory>\n", argv[0]);
		return -1;
	}

	char *smapfile = argv[1], *ifofile = argv[2], *aobdir = argv[3], *outdir = argv[4];

	struct sectormap *map = read_sector_map(smapfile);
	if (!map)
	{
		fprintf(stderr, "Fatal error: failed to read sector map file\n");
		return -1;
	}

	struct stat ifostat;
	if (stat(ifofile, &ifostat))
	{
		perror("stat");
		return -1;
	}
	printf("File size: %8.8x (%8.8x sectors)\n", ifostat.st_size, ifostat.st_size/DVD_VIDEO_LB_LEN);

	fd = open(ifofile, 0, 'r');
	if (!fd)
	{
		perror("open");
		return -1;
	}

	p_blocks = malloc(ifostat.st_size);
	if (!p_blocks)
	{
		perror("malloc");
		return -1;
	}

	int ret = read(fd, p_blocks, ifostat.st_size);
	close(fd);
	if (ret < ifostat.st_size)
	{
		fprintf(stderr, "Fatal error: read fewer bytes than requested\n");
		return -1;
	}

	struct ifo_header ifohdr;
	ret = read_ifo_header(p_blocks, &ifohdr);
	if (!ret)
	{
		fprintf(stderr, "Fatal error reading IFO header\n");
		return -1;
	}

	printf("Version                : 0x%4.4x\n", ifohdr.version);
	printf("VTS Category equiv?    : 0x%8.8x\n", ifohdr.vts_category);
	if ( (ifohdr.ifo_end_sector + 1)*DVD_VIDEO_LB_LEN != ifostat.st_size ) // Check IFO size
		fprintf(stderr, "Warning: FS filesize (%d) and IFO filesize (%d) do not agree", ifostat.st_size, (ifohdr.ifo_end_sector + 1) * DVD_VIDEO_LB_LEN);
	printf("\n");

	// Cells
	uint16_t i_ncells;
	ptr = p_blocks+DVD_VIDEO_LB_LEN;
	memcpy(&i_ncells, ptr, 2);
	B2N_16(i_ncells);
	printf("Number of cells        : %d\n", i_ncells);

	ptr += 8;    // Points to start of second sector title records table
	for( i = 0; i < i_ncells; i++ ) {
		uint8_t titleno;
		struct ifo_cell ifocell;
		read_ifo_cell(ptr + 8*i, p_blocks + DVD_VIDEO_LB_LEN, &ifocell);
		titleno = (ifocell.flags & 0x0F000000)>>24;
		printf("Title %2.2d               : Flags 0x%8.8x -> ", titleno, ifocell.flags);
		if ( ifocell.flags & 0x00000100 ) printf("MLP");
		else printf("LPCM");
		printf("\n");
		printf("                         Offset %8.8x\n", ifocell.offset);
		printf("                         %d tracks\n", ifocell.ntracks);
		printf("                         Length %2.2d:%2.2d:%2.2d.%3.3d\n", ifocell.total_length.hour, ifocell.total_length.min, ifocell.total_length.sec, ifocell.total_length.msec);
		printf("                         Table offsets:");
		for (j = 0; j < 4; j++)
			printf(" %4.4x", ifocell.table_offsets[j]);
		printf("\n");

		/*
		printf("                         Track Information:\n");
		for (j = 0; j < ifocell.ntracks; j++) {
			if ( ifocell.table_offsets[1])
				printf("                           Track %2.2d", ifocell.tbl2[j].trackno);
			else
				printf("                           Row %2.2d", j);

			if ( ifocell.table_offsets[1])
			{
				printf(", Flags 0x%4.4x%s, ", ifocell.tbl2[j].flags, (ifocell.tbl2[j].flags&0xc000)==0xc000?" -> First":"");
		        printf("@ %2.2d:%2.2d:%2.2d.%3.3d, ", ifocell.tbl2[j].start_time.hour, ifocell.tbl2[j].start_time.min, ifocell.tbl2[j].start_time.sec, ifocell.tbl2[j].start_time.msec);
		        printf("dur %2.2d:%2.2d:%2.2d.%3.3d", ifocell.tbl2[j].duration.hour, ifocell.tbl2[j].duration.min, ifocell.tbl2[j].duration.sec, ifocell.tbl2[j].duration.msec);

			}

			if (ifocell.table_offsets[2])
			{
				printf(", %8.8x -> ", ifocell.tbl3[j].start);
				printf("%8.8x (%8.8x)", ifocell.tbl3[j].stop, ifocell.tbl3[j].stop-ifocell.tbl3[j].start+1);
			}
			printf("\n");
		}
		*/

		if (!ifocell.table_offsets[2])
			fprintf(stderr, "No table 3 found, cannot extract tracks\n");
		else
		{
			uint32_t aoboffset, trackoffset, tracklen;
			char *ifoname, *outfile, outfilefmt[OUTFILELEN];

			ifoname = ifofile + strlen(ifofile);  // points to \0
			while (ifoname[0] != '/' && ifoname >= ifofile)
				ifoname--;
			ifoname++;

			while (map->prev)
				map = map->prev;
			while (strcmp(map->name, ifoname) && map->next)
				map = map->next;
			if (strcmp(map->name, ifoname))
			{
				fprintf(stderr, "Could not find '%s' in the sector map, unable to continue\n", ifoname);
			}
			else
			{
				printf("Found '%s' at sector 0x%8.8d\n", map->name, map->offset);
				aoboffset = map->offset;
				// Rewind sector map
				while (map->prev)
					map = map->prev;
				// Start processing tracks
				for (j = 0; j < ifocell.ntracks; j++)
				{
					trackoffset = ifocell.tbl3[j].start + aoboffset + ifohdr.vob_start_sector;
					tracklen = ifocell.tbl3[j].stop - ifocell.tbl3[j].start + 1;
					// Set output file name
					outfile = malloc(OUTFILELEN + strlen(outdir) + 3);
					if (!outfile)
					{
						perror("malloc");
						continue;
					}
					strcpy(outfile, outdir);
					if (outfile[strlen(outfile) - 1] != '/')
						strcat(outfile, "/");
					sprintf(outfilefmt, OUTFILEFMT, titleno, j+1);
					strcat(outfile, outfilefmt);
					strcat(outfile, "mpeg");
					int outfd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
					// Find the file containing this track
					while (trackoffset >= map->offset + map->sectors && map->next)
						map = map->next;
					if (trackoffset >= map->offset + map->sectors)
					{
						fprintf(stderr, "Cannot find file containing track %d\n", j + 1);
						continue;
					}
					trackoffset -= map->offset;
					// Open AOB
					char *aobpath = joinpath(aobdir, map->name);
					fd = open(aobpath, O_RDONLY);
					// Start copying the track
					lseek(fd, trackoffset * DVD_VIDEO_LB_LEN, SEEK_SET);
					printf("Found track %d in '%s' at offset 0x%8.8x, %d bytes, will write to '%s'\n", j + 1, aobpath, trackoffset, tracklen * DVD_VIDEO_LB_LEN, outfile);
					free(aobpath);
					while (tracklen > 0)
					{
						if (trackoffset == map->sectors)
						{
							if (!map->next)
							{
								fprintf(stderr, "Error: reached end of disc within a track!");
								break;
							}
							if (map->sectors + map->offset != map->next->offset)
							{
								fprintf(stderr, "Error: gap in disc detected within a track!");
								break;
							}
							trackoffset = 0;
							map = map->next;
							aobpath = joinpath(aobdir, map->name);
							close(fd);
							fd = open(aobpath, O_RDONLY);
							printf("Continuing with '%s'\n", aobpath);
							free(aobpath);
						}
						// FIXME check read length
						char buf[DVD_VIDEO_LB_LEN];
						read(fd, buf, DVD_VIDEO_LB_LEN);
						write(outfd, buf, DVD_VIDEO_LB_LEN);
						tracklen--;
					}
					close(fd);
					close(outfd);
				}
			}
		}

		if (ifocell.tbl2)
			free(ifocell.tbl2);
		if (ifocell.tbl3)
			free(ifocell.tbl3);
	}

	free(p_blocks);
	free_sector_map(map);

	return 0;
}

