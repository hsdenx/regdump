/*
 * (C) Copyright 2011
 * Heiko Schocher, DENX Software Engineering, hs@denx.de.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 *
 * gcc regdump.c -o regdump
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <termios.h>
#include <sys/mman.h>

#define FATAL do { fprintf(stderr, "Error at line %d, file %s (%d) [%s]\n", \
  __LINE__, __FILE__, errno, strerror(errno)); exit(1); } while(0)

#if 0
#define debug(fmt,arg...) \
        printf("%s %s: " fmt,__FILE__, __FUNCTION__, ##arg)
#else
#define debug(fmt,arg...) \
        do { } while (0)
#endif

#define MAP_SIZE 4096UL
#define MAP_MASK (MAP_SIZE - 1)

#define RD_LIST_MAX_NAME	50
#define RD_MAX_COMMENT_LEN	200
#define RD_MAX_LINE_LEN		300

/* List managment */
typedef struct _listhead {
	char name[RD_LIST_MAX_NAME];
	struct _list *list;
	struct _list *listend;
};

typedef struct _list {
	struct _listhead *head;
	struct _list *prev;
	struct _list *next;
	char name[RD_LIST_MAX_NAME];
	void *data;
};

typedef struct _headdata {
	unsigned long reg;
	int	bit;
	struct _listhead *head;
};

typedef struct _regdata {
	unsigned long	from;
	unsigned long	to;
	unsigned long	defval;
	char *comment;
};

typedef struct _regdump_data {
	char	name[100];
	struct _listhead	*head; /* list of registers */
	struct _listhead	*headreg; /* list of bitdefinitions for one register */
	int			read;
	int	verbose;
};

static int delay(long msec)
{
	struct timeval timeout;

	timeout.tv_sec = msec / 1000;
	timeout.tv_usec = msec % 1000;
	select (0, NULL, NULL, NULL, &timeout);
}
static struct _listhead *rd_create_list(char *name)
{
	struct _listhead *head;

	head = calloc(1, sizeof(struct _listhead));
	if (!head)
		return NULL;

	if (strlen(name) >= RD_LIST_MAX_NAME) {
		printf("%s: Namelen %d too long\n", __func__, strlen(name));
		return NULL;
	}
	strcpy(head->name, name);
	return head;
}

static struct _list *rd_list_search(struct _listhead *head, char *name)
{
	struct _list *tmp = head->list;

	while (tmp != NULL) {
		if ((strncmp(tmp->name, name, strlen(name)) == 0) && (strlen(name) == strlen(tmp->name)))
			return tmp;
		tmp = tmp->next;
	}
	return NULL;
}

static int rd_list_add(struct _listhead *head, char *name, void *data)
{
	struct _list *tmp;
	struct _list *tmp2;

	tmp = rd_list_search(head, name);
	if (tmp) {
		printf("%s name: %s in list\n", __func__, name, tmp->name);
		return 1;
	}
	tmp = calloc(1, sizeof(struct _list));

	tmp->head = head;
	tmp->data = data;
	strncpy(tmp->name, name, strlen(name));
	/* If list is First element */
	if (head->list == NULL) {
		head->list = tmp;
		head->listend = tmp;
		return 0;
	}

	tmp2 = head->listend;
	head->listend = tmp;
	tmp2->next = tmp;
	tmp->prev = tmp2;
	return 0;
}

static unsigned long rd_get_value(unsigned long val, struct _list *list)
{
	struct _regdata	*regdat;
	int	count;
	unsigned long	start;
	unsigned long	ret = 0;
	int	i = 0;

	regdat = list->data;
	if (!regdat) {
		printf("%s: no data\n", __func__);
		return 0;
	}
	count = regdat->to - regdat->from + 1;
	start = regdat->from;
	while (count > 0) {
		ret |= ((val >> (start + i)) & 0x01) << i;
		count --;
		i++;
	}
	return ret;
}

static char *rd_get_comment(struct _list *list)
{
	struct _regdata	*regdat;

	regdat = list->data;
	if (!regdat)
		return NULL;

	if (regdat->comment)
		return regdat->comment;

	return NULL;
}

static struct _list *regdump_search_list(struct _listhead *head, char *name)
{
	struct _list *list;

	list = rd_list_search(head, name);
	if (!list) {
		printf("name %s not found\n", name);
	}
	return list;
}

static int regdump_read_mem(unsigned long reg, unsigned long *val, int read)
{
	int fd;
	int ret = -1;
	void *map_base, *virt_addr;

	if((fd = open("/dev/mem", O_RDWR | O_SYNC)) == -1) {
		FATAL;
		close(fd);
		return ret;
	}
	/* Map one page */
	map_base = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, reg & ~MAP_MASK);

	if(map_base == (void *) -1) {
		FATAL;
		close(fd);
		return ret;
	}

	virt_addr = map_base + (reg & MAP_MASK);
	switch(read) {
		case 1:
			*val = *((unsigned char *) virt_addr);
			break;
		case 2:
			*val = *((unsigned short *) virt_addr);
			break;
		case 4:
			*val = *((unsigned long *) virt_addr);
			break;
		default:
			printf("unknown read mode: %d\n", read);
			munmap(0, MAP_SIZE);
			close(fd);
			return ret;
			break;
	}
	munmap(0, MAP_SIZE);
	close(fd);
	return 2;
}

static int regdump_is_comment(FILE *st, char *comment)
{
	int ret;

	/* read one char */
	ret = fread(comment, 1, 1, st);

	if (ret != 1) {
		comment[0] = 0;
		return -1;
	}

	if (comment[0] != '#') {
		comment[0] = 0;
		fseek(st, -1, SEEK_CUR);
		return 0;
	}
	fgets(comment, RD_MAX_COMMENT_LEN, st);
	printf("%s", comment);

	return 1;
}

static int regdump_read_line(struct _listhead *head, FILE *st, char *name, unsigned long *reg, char *comment)
{
	int ret = 0;
	char line[RD_MAX_LINE_LEN];
	char *tmp;

	ret = regdump_is_comment(st, comment);
	if (ret < 0)
		return 0;
	while (ret == 1)
		ret = regdump_is_comment(st, comment);

	tmp = fgets(line, RD_MAX_LINE_LEN, st);
	if (!tmp)
		return 0;

	ret = sscanf (line, "%s %lx %s\n", name, reg, comment);
	return ret;
}

static int regdump_read_value(struct _listhead *head, FILE *st, char *name, unsigned long *reg, int read)
{
	char	comment[RD_MAX_COMMENT_LEN];
	int	ret = 0;

	ret = regdump_read_line(head, st, name, reg, comment);
	if ((ret < 1) || (ret > 3))
		return ret;

	if (ret == 1)
		read = 1;

	if (comment[0] != 0)
		printf("%s %lx %s\n", name, *reg, comment);

	if (read) {
		struct _list *list;
		struct _headdata *headdata;
		int tmp;

		list = regdump_search_list(head, name);
		if (!list) {
			printf("%s: name %s not found\n", name);
			return -1;
		}
		headdata = list->data;
		ret = regdump_read_mem(headdata->reg, reg, headdata->bit / 8);
	}
	return ret;
}

// ret -1 -> err
// ret = 0 -> "}" gefunden
// ret > 0 wieviele spalten gefunden
static int rd_read_register_line(FILE *st, char *name, unsigned long *from, unsigned long *to, unsigned long *def, char *comment)
{
	char line[RD_MAX_LINE_LEN];
	char *tmp;
	int ret = 0;

	ret = regdump_is_comment(st, comment);
	if (ret < 0)
		return ret;
	while (ret == 1)
		ret = regdump_is_comment(st, comment);
	if (ret < 0)
		return ret;

	tmp = fgets(line, RD_MAX_LINE_LEN, st);
	if (!tmp) {
		return -1;
	}

	sscanf(line, "%s", name);
	if (name[0] == '}') {
		return 0;
	}

	memset(comment, 0, RD_MAX_COMMENT_LEN);
	ret = sscanf (line, "%s %d %d %x %199c\n", name, from, to, def, comment);
	if (ret == 5) {
		int len = strlen(comment);

		/* remove return */
		comment[len] = 0;
		comment[len- 1] = 0;
	}
	return ret;
}

static int regdump_load_registerdescrition(struct _regdump_data *apl, char *filename)
{
	FILE	*stream;
	struct _headdata 	*headdata;
	struct _regdata		*regdat;
	struct _list		*list;
	char	name[RD_LIST_MAX_NAME];
	char	regname[RD_LIST_MAX_NAME];
	unsigned long		addr;
	unsigned long		from;
	unsigned long		to;
	unsigned long		def;
	char comment[RD_MAX_LINE_LEN];
	int	ret;
	int	i;

	stream = fopen (filename, "r");
	if (!stream) {
		printf("%s: could not open %s\n", apl->name, filename);
		exit (2);
	}

	fscanf (stream, "%s\n", name);
	apl->head = rd_create_list(name);
	if (!apl->head) {
		printf("%s: could not create list: %s\n", apl->name, name);
		exit (3);
	}

	ret = fscanf (stream, "%s\n", name);
	if (strcmp(name, "{") != 0) {
		printf("missing open bracket\n");
		exit(5);
	}

	/* scan Registername */
	ret = fscanf (stream, "%s\n", name);
	while (ret == 1) {
		i = 0;
		while (strcmp(name, "{") != 0) {
			int bit;

			fscanf (stream, "%x %d\n", &addr, &bit);
			debug("name: %s addr: %x bit: %d\n", name, addr, bit);
			apl->headreg = rd_create_list(name);
			if (!apl->headreg) {
				printf("%s: could not create list: %s\n", apl->name, name);
				exit (5);
			}
			headdata = calloc(1, sizeof(struct _headdata));
			if (!headdata) {
				printf("%s: could not create headdata\n", apl->name);
				exit (6);
			}
			headdata->reg = addr;
			headdata->bit = bit;
			headdata->head = apl->headreg;
			ret = rd_list_add(apl->head, name, (void *)headdata);
			i++;
			if (i > 1) {
				strncpy(regname, name, strlen(name));
				regname[strlen(name)] = 0;
			}
			fscanf (stream, "%s\n", name, bit);
		}
	
		/* read register bit values */
		ret = rd_read_register_line(stream, name, &from, &to, &def, comment);
		while (ret > 0) {
			if (ret < 4) {
				printf("%s: missing data\n", apl->name);
				exit (5);
			}
			regdat = calloc(1, sizeof(struct _regdata));
			if (!regdat) {
				printf("%s: could not alloc mem for data\n", apl->name);
				exit (5);
			}
			regdat->from = from;
			regdat->to = to;
			regdat->defval = def;
			if (ret == 5) {
				int len = strlen(comment);

				regdat->comment = calloc(1, len + 1);
				if (!regdat->comment) {
					printf("%s: could not alloc mem for data\n", apl->name);
					exit (6);
				}
				strncpy(regdat->comment, comment, len);
			}
			ret = rd_list_add(apl->headreg, name, (void *)regdat);
			if (ret != 0) {
				printf("%s: list add error\n", apl->name);
				exit (5);
			}
			ret = rd_read_register_line(stream, name, &from, &to, &def, comment);
		}
		/* fill the identical also */
		if (i > 1) {
			struct _headdata *dstd;
			struct _headdata *srcd;
			struct _listhead *dsth;
			struct _listhead *srch;
			struct _list	*dst;
			struct _list	*src;

			list = rd_list_search(apl->head, regname);
			if (!list) {
				printf("Could not found %s\n", regname);
			}
			/* search the previous ... */
			i--;
			while (i > 0) {
				/* ... and add the register descrition */
				dst = list->prev;
				dstd = dst->data;
				dsth = dstd->head;
				srcd = list->data;
				srch = srcd->head;
				dsth->list = srch->list;
				i--;
				list = dst;
			}
		}
		ret = fscanf (stream, "%s\n", name);
		if (strcmp(name, "}") == 0)
			ret = 0;
	}
	fclose (stream);
	return 0;
}

/* Main */
int main(int argc, char *argv[])
{
	struct _regdump_data	*apl;
	struct _list		*list;
	struct _list		*tmp;
	FILE			*stream;
	int			ret;
	unsigned long		reg;
	char			name[RD_LIST_MAX_NAME];
	char			regname[RD_LIST_MAX_NAME];
	unsigned long		addr;
	unsigned long		from;
	unsigned long		to;
	unsigned long		def;
	int	i;
	struct _regdata		*regdat;
	struct _headdata 	*headdata;

	if ( argc < 3 ) {
		printf( "Usage: %s registerdescriptionfile regfile [memaccess 1=c 2=s 4=l]\n", argv[0] );
		exit (1);
	}
	apl = calloc(1, sizeof(struct _regdump_data));
	if (!apl) {
		printf("%s: Could not get mem for apl data\n", argv[0]);
		exit(1);
	}
	strcpy(apl->name, argv[0]);

	/* load the register descrition file */
	regdump_load_registerdescrition(apl, argv[1]);

	/* load the regfile */
	stream = fopen (argv[2], "r");
	if (!stream) {
		printf("%s: could not open %s\n", apl->name, argv[2]);
		exit (2);
	}

	if (argc > 3) {
                apl->read = atoi(argv[3]);
		printf("reading values from /dev/mem\n");
	}
	apl->verbose = 1;
	if (argc > 4) {
                apl->verbose = atoi(argv[4]);
		printf("verbose: %d", apl->verbose);
	}
	if (apl->read == 8) {
		struct _list		*list;
		struct _headdata	*headdata;

		list = apl->head->list;
		while (list != NULL) {
			headdata = list->data;
			printf("%s %lx %d\n", list->name, headdata->reg, headdata->bit);
			list = list->next;
		}
		exit(0);
	}

	i = regdump_read_value(apl->head, stream, name, &reg, apl->read);
	while (i > 0) {
		printf("--------------------------------------\n");
		list = regdump_search_list(apl->head, name);
		if (!list) {
			printf("%s: name %s not found\n", argv[0], name);
			exit (3);
		}
		headdata = list->data;
		apl->headreg = headdata->head;
		printf("%s@%x %d bit val: %lx\n", apl->headreg->name, headdata->reg, headdata->bit, reg);
		list = apl->headreg->list;
		if (apl->verbose) {
			while (list) {
				unsigned long val;
				char *comment;

				val = rd_get_value(reg, list);
				comment = rd_get_comment(list);
				if (comment)
					printf("%s %x : %s\n", list->name, val, comment);
				else
					printf("%s %x\n", list->name, val);
				list = list->next;
			}
		}
		i = regdump_read_value(apl->head, stream, name, &reg, apl->read);
	}

	fclose (stream);

	exit (0);
}
