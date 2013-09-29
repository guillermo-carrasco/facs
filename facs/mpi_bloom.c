#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64
#include <math.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include<fcntl.h>
#include<unistd.h>
#include<sys/stat.h>
#include<sys/time.h>
#include<sys/mman.h>
#include<sys/types.h>

#include "tool.h"
#include "prob.h"
#include "bloom.h"
#include "query.h"
#include "check.h"
#include "hashes.h"
#include "mpi_bloom.h"
#include<omp.h>
#include<mpi.h>
/*-------------------------------------*/
static int mpicheck_usage (void)
{
  fprintf (stderr, "\nUsage: mpirun -n Nr_of_nodes ./facs [options]\n");
  fprintf (stderr, "Options:\n");
  fprintf (stderr, "\t-r reference Bloom filter to query against\n");
  fprintf (stderr, "\t-q FASTA/FASTQ file containing the query\n");
  fprintf (stderr, "\t-l input list containing all Bloom filters,\
           one per line\n");
  fprintf (stderr, "\t-t threshold value\n");
  fprintf (stderr, "\t-f report output format, valid values are:\
           'json' and 'tsv'\n");
  fprintf (stderr, "\t-s sampling rate, default is 1 so it reads the whole\
           query file\n");
  fprintf (stderr, "\n");
  exit(1);
}
/*-------------------------------------*/
mpi_main (int argc, char **argv)
{
  if (argc<3)
  {
	return mpicheck_usage();
  }
/*------------variables----------------*/
  double tole_rate = 0, sampling_rate = 1;
  char *ref = NULL, *list = NULL, *target_path = NULL, *position = NULL,  *source = NULL, *report_fmt = "json";
  int opt, ntask = 0, mytask = 0;
  BIGCAST share=0, offset=0;
  char type = '@';
  bloom *bl_2 = NEW (bloom);
  Queue *head = NEW (Queue), *tail = NEW (Queue), *head2 = head;
  head->location = NULL;
  head->next = tail;
/*------------get opt------------------*/
  while ((opt = getopt (argc, argv, "s:t:r:o:q:l:f:h")) != -1)
  {
      switch (opt)
      {
        case 't':
          tole_rate = atof(optarg);
          break;
        case 's':
          sampling_rate = atof(optarg);
          break;
        case 'o':
          target_path = optarg;
          break;
        case 'q':
          source = optarg;
          break;
        case 'r':
          ref = optarg;
          break;
        case 'l':
          list = optarg;
          break;
        case 'f': // "json", "tsv" or none
          (optarg) && (report_fmt = optarg, 1);
          break;
        case 'h':
          return mpicheck_usage();
          break;
        case '?':
          printf ("Unknown option: -%c\n", (char) optopt);
          return mpicheck_usage();
          break;
      }
  }
  if (!target_path && !source)
  {
  	fprintf (stderr, "\nPlease, at least specify a bloom filter (-r) and a query file (-q)\n");
        exit (-1);
  }
  if (target_path == NULL)
  {
        target_path = argv[0];
  } 
  if ((zip = gzopen (query, "rb")) < 0)
  {
        fprintf(stderr, "%s\n", strerror(errno));
        exit(EXIT_FAILURE);
  }
  if (strstr (query, ".fastq") != NULL || strstr (query, ".fq") != NULL)
        type = '@';
  else
        type = '>';
  /*initialize emtpy string for query*/
  position = (char *) calloc ((2*ONEG + 1), sizeof (char));
  /*----------MPI initialize------------*/
  MPI_Init (&argc, &argv);
  MPI_Comm_size (MPI_COMM_WORLD, &ntask);
  MPI_Comm_rank (MPI_COMM_WORLD, &mytask);
  share = struc_init (source,ntask,mytask);
  F_set *File_head = make_list (bloom_filter, list);
  File_head->reads_num = 0;
  File_head->reads_contam = 0;
  File_head->hits = 0;
  File_head->all_k = 0;
  File_head->filename = bloom_filter;
  load_bloom (File_head->filename, bl_2);	//load a bloom filter
  if (tole_rate == 0)
  {
  	tole_rate = mco_suggestion (bl_2->k_mer);
  }
  while (offset<share)
  {
        offset+= gz_read_mu();
	// put offset += ntask* share inside
	get_parainfo (position, head, type);
        head = head->next;
#pragma omp parallel
  	{
#pragma omp single nowait
	{
	  	while (head != tail)
		{
#pragma omp task firstprivate(head)
		{
			printf ("position->%0.10s\n", head->location);
	      		if (head->location != NULL)
                	{
                        	read_process (bl_2, head, tail, File_head, sampling_rate, tole_rate, mode, type);
			}
		}
	      	head = head->next;
		}
      	}	
	}
     		memset (position, 0, strlen(position));
      		share -= buffer;
  }
  printf ("finish processing...\n");
  MPI_Barrier (MPI_COMM_WORLD);	//wait until all nodes finish
  gather (File_head);			//gather info from all nodes
  if (mytask == 0)		
  {
  	return report(File_head, query, report_fmt, target_path, prob_suggestion(bl_2->k_mer));
  }
  MPI_Finalize ();
  return 0;
}
/*-------------------------------------*/
void struc_init (char *filename, int ntask, int mytask)
{
  
  BIGCAST total_size = get_size(filename);
  BIGCAST share = 0;
  if (total_size<=2*ONEG)
  {
	if (mytask==0)
	{
		share = total_size;
  	}
  }
  else
  {
  	share = total_size / ntask;	//every task gets an euqal piece
  	if (total_size%ntask!=0 && mytask==(ntask-1))
  	{
  		share += (total_size % ntask);	//last node takes extra job
  	}
  }
  return share;
}
/*-------------------------------------*/
/*current sacrifice file mapping, use gzip instead*/
char *ammaping (char *source)
{
  int src;
  char *sm;

  if ((src = open (source, O_RDONLY | O_LARGEFILE)) < 0)
    {
      perror (" open source ");
      exit (EXIT_FAILURE);
    }

  if (fstat (src, &statbuf) < 0)
    {
      perror (" fstat source ");
      exit (EXIT_FAILURE);
    }

  printf ("share->%d PAGES per node\n", share);

  if (share >= CHUNK)
    buffer = CHUNK;
  else
    buffer = share;
  printf ("total pieces->%d\n", total_piece);
  printf ("PAGE->%d\n", PAGE);
  printf ("node %d chunk size %d buffer size %d offset %d\n", mytask, CHUNK,
	  buffer, offset);

  sm = mmap (0, buffer * PAGE, PROT_READ, MAP_SHARED | MAP_NORESERVE, src, offset * PAGE);	//everytime we process a chunk of data

  //sm = mmap (0,share*PAGE, PROT_READ, MAP_SHARED | MAP_NORESERVE,src, offsetmytask*share*PAGE); //last time we process the rest

  if (MAP_FAILED == sm)
    {
      perror (" mmap source ");
      exit (EXIT_FAILURE);
    }

  return sm;
}

/*-------------------------------------*/
int gather (F_set *File_head)
{
  printf ("gathering...\n");
  if (mytask == 0)
  {
        // The master thread will need to receive all computations from all other threads.
  	MPI_Status status;
        // MPI_Recv(void *buf, int count, MPI_DAtatype datatype, int source, int tag, MPI_Comm comm, MPI_Status *status)
        // We need to go and receive the data from all other threads.
        // The arbitrary tag we choose is 1, for now.
  	int i = 0;
     	for (i = 1; i < ntask; i++)
      	{
		BIGCAST temp, temp2, temp3, temp4;
		MPI_Recv (&temp, 1, MPI_LONG_LONG_INT, i, 1, MPI_COMM_WORLD, &status);
	  	MPI_Recv (&temp2, 2, MPI_LONG_LONG_INT, i, 1, MPI_COMM_WORLD, &status);
	  	MPI_Recv (&temp3, 3, MPI_LONG_LONG_INT, i, 1, MPI_COMM_WORLD, &status);
		MPI_Recv (&temp4, 4, MPI_LONG_LONG_INT, i, 1, MPI_COMM_WORLD, &status);
	  	printf ("RECEIVED %lld from thread %d\n", temp, i);
	  	File_head->reads_num += temp;
	  	File_head->reads_contam += temp2;
	  	File_head->hits += temp3;
		File_head->all_k+=temp4;
		
      	}
  }
  else
  {
      	// We are finished with the results in this thread, and need to send the data to thread 1.
      	// MPI_Send(void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm)
      	// The destination is thread 0, and the arbitrary tag we choose for now is 1.
      	MPI_Send (File_head->reads_num, 1, MPI_LONG_LONG_INT, 0, 1, MPI_COMM_WORLD);
      	MPI_Send (File_head->reads_contam, 2, MPI_LONG_LONG_INT, 0, 1, MPI_COMM_WORLD);
      	MPI_Send (File_head->hits, 3, MPI_LONG_LONG_INT, 0, 1, MPI_COMM_WORLD);
        MPI_Send (File_head->all_k, 4, MPI_LONG_LONG_INT, 0, 1, MPI_COMM_WORLD);
  }
  return 1;
}

/*-------------------------------------*/

