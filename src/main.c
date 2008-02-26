/*
 *  xnec2c - GTK2-based version of nec2c, the C translation of NEC2
 *  Copyright (C) 2003-2006 N. Kyriazis <neoklis<at>mailspeed.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * Initial main.c file generated by Glade. Edit as required.
 * Glade will not overwrite this file.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gtk/gtk.h>
#include <sys/wait.h>
#include <locale.h>

#include "interface.h"
#include "support.h"
#include "fork.h"
#include "xnec2c.h"

/* Main (structure) window */
GtkWidget *main_window = NULL;

/* Main window freq spinbutton */
GtkSpinButton *mainwin_frequency;

/* Pointers to input/output files */
FILE *input_fp  = NULL;
char infile[81] = "";

/* Parameters for projecting structure to Screen */
projection_parameters_t structure_proj_params;

/* Parameters for projecting radiation patterns to Screen */
projection_parameters_t rdpattern_proj_params;

/* Strucrure current data */
extern crnt_t crnt;

/* Some graphics contexts */
GdkGC
  *white_gc = NULL,
  *black_gc = NULL,
  *plot_gc  = NULL;

/* Drawing area widgets */
extern GtkWidget *structure_drawingarea;
extern GtkWidget *plots_drawingarea;
extern GtkWidget *rdpattern_drawingarea;
extern GtkWidget *freqplots_window;
extern GtkWidget *rdpattern_window;

/* Structure rotation spin buttons */
GtkSpinButton *rotate_structure  = NULL;
GtkSpinButton *incline_structure = NULL;

/* Structure drawing pixmap */
extern GdkPixmap *structure_pixmap;

/* Data needed during prog execution */
extern calc_data_t calc_data;

/* Saved data buffer */
extern save_t save;

/* Near field data */
extern near_field_t near_field;

/* Frequency loop idle function tag */
extern gint floop_tag;

static void sig_handler(int signal);

/* Child process pid returned by fork() */
pid_t child = (pid_t)(-1);

/* Number of forked child processes */
int nchild = 0;

/* Forked process data */
forkpc_t **forkpc = NULL;

/* Program forked flag */
gboolean FORKED = FALSE;

/* Commands between parent and child processes */
char *comnd[] = FORK_CMNDS;

/*------------------------------------------------------------------------*/

  int
main (int argc, char *argv[])
{
  /* getopt() variables */
  extern char *optarg;
  extern int optind, opterr, optopt;
  int option, idx;

  /*** signal handler related code ***/
  /* new and old actions for sigaction() */
  struct sigaction sa_new, sa_old;

  /* initialize new actions */
  sa_new.sa_handler = sig_handler;
  sigemptyset( &sa_new.sa_mask );
  sa_new.sa_flags = 0;

  /* register function to handle signals */
  sigaction( SIGINT,  &sa_new, &sa_old );
  sigaction( SIGSEGV, &sa_new, 0 );
  sigaction( SIGFPE,  &sa_new, 0 );
  sigaction( SIGTERM, &sa_new, 0 );
  sigaction( SIGABRT, &sa_new, 0 );
  sigaction( SIGCHLD, &sa_new, 0 );

  /* process command line options */
  calc_data.nfork = 1;
  while( (option = getopt(argc, argv, "i:j:hv") ) != -1 )
  {
	switch( option )
	{
	  case 'i' : /* specify input file name */
		if( strlen(optarg) > 80 )
		  stop( "Input file name too long", 1 );
		strcpy( infile, optarg );
		break;

	  case 'j' : /* number of child processes = num of processors */
		calc_data.nfork = atoi( optarg );
		break;

	  case 'h' : /* print usage and exit */
		usage();
		exit(0);

	  case 'v' : /* print xnec2c version */
		printf( "%s %s\n", PACKAGE, VERSION );
		exit(0);

	} /* end of switch( option ) */

  } /* while( (option = getopt(argc, argv, "i:o:hv") ) != -1 ) */

  /* When forking is useful, e.g. if more than 1 processor is
   * available, the parent process handles the GUI and delegates
   * calculations to the child processes, one per processor. The
   * requested number of child processes = number of processors */

  /* Allocate buffers for fork data */
  if( calc_data.nfork > 1 )
  {
	mem_alloc( (void *)&forkpc,
		calc_data.nfork * sizeof(forkpc_t *), "in main.c" );
	for( idx = 0; idx < calc_data.nfork; idx++ )
	{
	  forkpc[idx] = NULL;
	  mem_alloc( (void *)&forkpc[idx],
		  sizeof(forkpc_t), "in main.c" );
	}

	/* Fork child processes */
	for( idx = 0; idx < calc_data.nfork; idx++ )
	{
	  /* Make pipes to transfer data */
	  if( pipe( forkpc[idx]->p2ch_pipe ) == -1 )
	  {
		perror( "xnec2c: pipe()" );
		break;
	  }
	  if( pipe( forkpc[idx]->ch2p_pipe ) == -1 )
	  {
		perror( "xnec2c: pipe()" );
		break;
	  }

	  /* Fork child process */
	  if( (forkpc[idx]->chpid = fork()) == -1 )
	  {
		perror( "xnec2c: fork()" );
		puts( "xnec2c: Exiting after fatal error (fork() failed)" );
		exit(-1);
	  }
	  else
		child = forkpc[idx]->chpid;

	  /* Child get out of forking loop! */
	  if( CHILD ) Child_Process();

	  /* Ready to accept a job */
	  forkpc[nchild]->busy = 0;

	  /* Close unwanted pipe ends */
	  close( forkpc[idx]->p2ch_pipe[READ] );
	  close( forkpc[idx]->ch2p_pipe[WRITE] );

	  /* Set file descriptors for select() */
	  FD_ZERO( &forkpc[idx]->read_fds );
	  FD_SET( forkpc[idx]->ch2p_pipe[READ], &forkpc[idx]->read_fds );
	  FD_ZERO( &forkpc[idx]->write_fds );
	  FD_SET( forkpc[idx]->p2ch_pipe[WRITE], &forkpc[idx]->write_fds );

	  /* Count child processes */
	  nchild++;
	} /* for( idx = 0; idx < calc_data.nfork; idx++ ) */

	FORKED = TRUE;

  } /* if( calc_data.nfork > 1 ) */

  gtk_set_locale ();
  gtk_init (&argc, &argv);
  setlocale(LC_NUMERIC, "C");
  add_pixmap_directory (PACKAGE_DATA_DIR "/" PACKAGE "/pixmaps");

  /*
   * The following code was added by Glade to create one of each component
   * (except popup menus), just so that you see something after building
   * the project. Delete any components that you don't want shown initially.
   */
  main_window = create_main_window ();
  gtk_widget_show (main_window);
  mainwin_frequency = GTK_SPIN_BUTTON(lookup_widget(
		main_window, "main_freq_spinbutton") );

  gtk_widget_hide_all( lookup_widget(
		main_window, "main_hbox1") );
  gtk_widget_hide_all( lookup_widget(
		main_window, "main_hbox2") );
  gtk_widget_hide_all( lookup_widget(
		main_window, "main_view_menuitem") );
  gtk_widget_hide( lookup_widget(
		main_window, "structure_drawingarea") );

  structure_drawingarea = lookup_widget(
	  main_window, "structure_drawingarea");
  gtk_widget_add_events(
	  GTK_WIDGET(structure_drawingarea),
	  GDK_BUTTON_MOTION_MASK | GDK_BUTTON_PRESS_MASK );

  /* Make some graphics contexts */
  white_gc = main_window->style->white_gc;
  black_gc = main_window->style->black_gc;
  plot_gc  = gdk_gc_new( structure_drawingarea->window );

  /* Initialize structure projection angles */
  rotate_structure  = GTK_SPIN_BUTTON(lookup_widget(
		main_window, "main_rotate_spinbutton"));
  incline_structure = GTK_SPIN_BUTTON(lookup_widget(
		main_window, "main_incline_spinbutton"));
  structure_proj_params.Wr =
	gtk_spin_button_get_value(rotate_structure);
  structure_proj_params.Wi =
	gtk_spin_button_get_value(incline_structure);
  structure_proj_params.W_incr = PROJ_ANGLE_INCR;
  New_Structure_Projection_Angle();

  /* Open input file if specified */
  if( strlen(infile) > 0 )
	g_idle_add( Open_Input_File, NULL );

  gtk_main ();

  return 0;
}

/*-----------------------------------------------------------------------*/

static void sig_handler( int signal )
{
  switch( signal )
  {
	case SIGINT:
	  fprintf( stderr, "xnec2c: exiting via user interrupt\n" );
	  break;

	case SIGSEGV:
	  fprintf( stderr, "xnec2c: segmentation fault, exiting\n" );
	  break;

	case SIGFPE:
	  fprintf( stderr, "xnec2c: floating point exception, exiting\n" );
	  break;

	case SIGABRT:
	  fprintf( stderr, "xnec2c: abort signal received, exiting\n" );
	  break;

	case SIGTERM:
	  fprintf( stderr, "xnec2c: termination request received, exiting\n" );
	  break;

	case SIGCHLD:
	  if( !FORKED )
	  {
		fprintf( stderr, "xnec2c: no child processes, ignoring SIGCHLD\n" );
		return;
	  }
	  fprintf( stderr, "xnec2c: child process exited\n" );
	  wait(NULL);
	  if( isFlagSet(MAIN_QUIT) ) return;

  } /* switch( signal ) */

  /* Kill child processes */
  if( FORKED && !CHILD )
	while( nchild )
	  kill( forkpc[--nchild]->chpid, SIGKILL );

  Close_File( &input_fp );
  if( CHILD )
	_exit( 0 );
  else
	exit( signal );

} /* end of sig_handler() */

/*------------------------------------------------------------------------*/

/* Functions for testing and setting/clearing flow control flags
 *
 *  See xfhell.h for definition of flow control flags
 */

/* An int variable holding the single-bit flags */
static long long int Flags = 0;

  int
isFlagSet( long long int flag )
{
  return( (Flags & flag) == flag );
}

  int
isFlagClear( long long int flag )
{
  return( (~Flags & flag) == flag );
}

  void
SetFlag( long long int flag )
{
  Flags |= flag;
}

  void
ClearFlag( long long int flag )
{
  Flags &= ~flag;
}

  void
ToggleFlag( long long int flag )
{
  Flags ^= flag;
}

/*------------------------------------------------------------------------*/

/* Tests for child process */
  gboolean
isChild(void)
{
  return( child == (pid_t)(0) );
}

/*------------------------------------------------------------------------*/

/* Open_Input_File()
 *
 * Opens NEC2 input file
 */

  gboolean
Open_Input_File( gpointer data )
{
  /* Clear all menu selections when new input file is opened */
  gtk_toggle_button_set_active(
	  GTK_TOGGLE_BUTTON(lookup_widget(main_window,
		  "main_currents_togglebutton")), FALSE );
  gtk_toggle_button_set_active(
	  GTK_TOGGLE_BUTTON(lookup_widget(main_window,
		  "main_charges_togglebutton")), FALSE );

  /* Close open windows */
  if( isFlagSet(PLOT_ENABLED) )
	gtk_widget_destroy( freqplots_window );
  if( isFlagSet(DRAW_ENABLED) )
	gtk_widget_destroy( rdpattern_window );
  gtk_check_menu_item_set_active(
	  GTK_CHECK_MENU_ITEM(lookup_widget(
		  main_window, "main_freqplots")), FALSE );
  gtk_check_menu_item_set_active(
	  GTK_CHECK_MENU_ITEM(lookup_widget(
		  main_window, "main_rdpattern")), FALSE );

  /* Stop freq loop */
  if( isFlagSet(FREQ_LOOP_RUNNING) )
	Stop_Frequency_Loop();

  /* Close open files if any */
  Close_File( &input_fp );

  /* Open NEC2 input file */
  Open_File( &input_fp, infile, "r");

  /* Read input file */
  ClearFlag( ALL_FLAGS );

  /* Suppress activity while input file opened */
  SetFlag( INPUT_PENDING );
  Read_Comments();
  Read_Geometry();
  Read_Commands();

  /* Ask child processes to read input file */
  if( FORKED )
  {
	int idx;

	for( idx = 0; idx < nchild; idx++ )
	{
	  Write_Pipe( idx, comnd[INFILE], strlen(comnd[INFILE]), TRUE );
	  Write_Pipe( idx, infile, strlen(infile), TRUE );
	}
  } /* if( FORKED ) */

  /* Allow redraws on expose events etc */
  ClearFlag( INPUT_PENDING );

  /* Initialize xnec2c */
  SetFlag( COMMON_PROJECTION );
  SetFlag( COMMON_FREQUENCY );
  SetFlag( MAIN_NEW_FREQ );
  SetFlag( FREQ_LOOP_INIT );
  SetFlag( OVERLAY_STRUCT );
  floop_tag = 0;
  save.last_freq = 0.0l;
  crnt.newer = crnt.valid = 0;
  near_field.newer = near_field.valid = 0;

  /* Projection at 45 deg rotation and inclination */
  New_Viewer_Angle( 45.0, 45.0, rotate_structure,
	  incline_structure, &structure_proj_params );
  New_Structure_Projection_Angle();

  /* Show current frequency */
  gtk_spin_button_set_value(
	  mainwin_frequency, (gdouble)calc_data.fmhz );

  /* Show main control buttons etc */
  gtk_widget_show_all( lookup_widget(
		main_window, "main_hbox1") );
  gtk_widget_show_all( lookup_widget(
		main_window, "main_hbox2") );
  gtk_widget_show_all( lookup_widget(
		main_window, "main_view_menuitem") );
  gtk_widget_show( lookup_widget(
		main_window, "structure_drawingarea") );

  return( FALSE );

} /* Open_Input_FIle() */

/*------------------------------------------------------------------------*/

