/*-----------------------------------------------------------------------------
 * Copyright (c) 1996 Southeastern Universities Research Association,
 *               Continuous Electron Beam Accelerator Facility
 *
 * This software was developed under a United States Government license
 * described in the NOTICE file included as part of this distribution.
 *
 *-----------------------------------------------------------------------------
 */

#include "StripDAQ.h"

#include <cadef.h>
#include <db_access.h>

typedef struct _StripDAQInfo
{
  Strip         strip;
  struct        _ChannelData
  {
    chid                        chan_id;
    evid                        event_id;
    double                      value;
    struct _StripDAQInfo        *this;
  } chan_data[STRIP_MAX_CURVES];
} StripDAQInfo;


/* ====== Prototypes ====== */
static void     addfd_callback          (void *, int, int);
static void     timeout_callback        (XtPointer, XtIntervalId *);
static void     work_callback           (XtPointer, int *, XtInputId *);
static void     connect_callback        (struct connection_handler_args);
static void     info_callback           (struct event_handler_args);
static void     data_callback           (struct event_handler_args);
static double   get_value               (void *);
static void     getDescriptionRecord    (char *name,char *description);

/*
 * StripDAQ_initialize
 */
StripDAQ                StripDAQ_initialize     (Strip strip)
{
  StripDAQInfo  *sca = NULL;
  int           status;
  int           i;

  
  if ((sca = (StripDAQInfo *)calloc (sizeof (StripDAQInfo), 1)) != NULL)
  {
    sca->strip = strip;
    status = ca_task_initialize ();
    if (status != ECA_NORMAL)
    {
      SEVCHK (status, "StripDAQ: Channel Access initialization error");
      free (sca);
      sca = NULL;
    }
    else {
      Strip_addtimeout (strip, 0.1, timeout_callback, strip);
      ca_add_fd_registration (addfd_callback, sca);
      for (i = 0; i < STRIP_MAX_CURVES; i++)
      {
        sca->chan_data[i].this    = sca;
        sca->chan_data[i].chan_id         = NULL;
      }
    }
  }

  return (StripDAQ)sca;
}


/*
 * StripDAQ_terminate
 */
void            StripDAQ_terminate      (StripDAQ the_sca)
{
  ca_task_exit ();
}



/*
 * StripDAQ_request_connect
 *
 *      Requests connection for the specified curve.
 */
int     StripDAQ_request_connect        (StripCurve curve, void *the_sca)
{
  StripDAQInfo  *sca = (StripDAQInfo *)the_sca;
  int           i;
  int           ret_val;
char *description=NULL; /* Albert */

  for (i = 0; i < STRIP_MAX_CURVES; i++)
    if (sca->chan_data[i].chan_id == NULL)
      break;

  if (ret_val = (i < STRIP_MAX_CURVES))
  {
    StripCurve_setattr (curve, STRIPCURVE_FUNCDATA, &sca->chan_data[i], 0);
    ret_val = ca_search_and_connect
      ((char *)StripCurve_getattr_val (curve, STRIPCURVE_NAME),
       &sca->chan_data[i].chan_id,
       connect_callback,
       curve);
    if (ret_val != ECA_NORMAL)
    {
      SEVCHK (ret_val, "StripDAQ: Channel Access unable to connect\n");
      fprintf
        (stderr, "channel name: %s\n",
         (char *)StripCurve_getattr_val (curve, STRIPCURVE_NAME));
      ret_val = 0;
    }
    else ret_val = 1;
  }
#if 1
	description=malloc(STRIP_MAX_NAME_CHAR); /* Albert */
	getDescriptionRecord(
	 (char *)StripCurve_getattr_val (curve, STRIPCURVE_NAME),
	 description);
	StripCurve_setattr (curve, STRIPCURVE_COMMENT, description, 0);
	free(description);
#endif
  ca_flush_io();


  return ret_val;
}


/*
 * StripDAQ_request_disconnect
 *
 *      Requests disconnection for the specified curve.
 */
int     StripDAQ_request_disconnect     (StripCurve     curve,
                                         void           *the_sca)
{
  struct _ChannelData   *cd;
  int                   ret_val;

  cd = (struct _ChannelData *) StripCurve_getattr_val
    (curve, STRIPCURVE_FUNCDATA);

  /* this will happen if a non-CA curve is submitted for disconnect */
  if (!cd) return 1;

  if (cd->event_id != NULL)
  {
    if ((ret_val = ca_clear_event (cd->event_id)) != ECA_NORMAL)
    {
      SEVCHK
        (ret_val, "StripDAQ_request_disconnect: error in ca_clear_event");
      ret_val = 0;
    }
    else
    {
      cd->event_id = NULL;
      ret_val = 1;
    }
  }
  ca_flush_io();
  
  if (cd->chan_id != NULL)
  {
    /* **** ca_clear_channel() causes info to be printed to stdout **** */
    if ((ret_val = ca_clear_channel (cd->chan_id)) != ECA_NORMAL)
    {
      SEVCHK
        (ret_val, "StripDAQ_request_disconnect: error in ca_clear_channel");
      ret_val = 0;
    }
    else
    {
      cd->chan_id = NULL;
      ret_val = 1;
    }
  }
  else ret_val = 1;

  ca_flush_io();
  return ret_val;
}


/*
 * addfd_callback
 *
 *      Add new file descriptors to select upon.
 *      Remove old file descriptors from selection.
 */
static void     addfd_callback  (void *data, int fd, int active)
{
  StripDAQInfo  *strip_ca = (StripDAQInfo *)data;
  if (active)
    Strip_addfd (strip_ca->strip, fd, work_callback, (XtPointer)strip_ca);
  else
    Strip_clearfd (strip_ca->strip, fd);
}


/*
 * work_callback
 *
 *      Gives control to Channel Access for a while.
 */
static void     work_callback           (XtPointer      BOGUS(1),
                                         int            *BOGUS(2),
                                         XtInputId      *BOGUS(3))
{
  ca_pend_event (STRIP_CA_PEND_TIMEOUT);
}

/*
 * timeout_callback
 */
static void     timeout_callback        (XtPointer ptr, XtIntervalId *pId)
{
  Strip strip = (Strip) ptr;
  ca_pend_event (STRIP_CA_PEND_TIMEOUT);
  Strip_addtimeout ( strip, 0.1, timeout_callback, strip );
}

/*
 * connect_callback
 */
static void     connect_callback        (struct connection_handler_args args)
{
  StripCurve            curve;
  struct _ChannelData   *cd;
  int                   status;

  curve = (StripCurve)(ca_puser (args.chid));
  cd = (struct _ChannelData *)StripCurve_getattr_val
    (curve, STRIPCURVE_FUNCDATA);

  switch (ca_state (args.chid))
  {
      case cs_never_conn:
        fprintf (stderr, "StripDAQ connect_callback: ioc not found\n");
        cd->chan_id = NULL;
        cd->event_id = NULL;
        Strip_freecurve (cd->this->strip, curve);
        break;
      
      case cs_prev_conn:
        fprintf
          (stderr,
           "StripDAQ connect_callback: IOC unavailable for %s\n",
           ca_name (args.chid));
        Strip_setwaiting (cd->this->strip, curve);
        break;
      
      case cs_conn:
        /* now connected, so get the control info if this is first time */
        if (cd->event_id == 0)
        {
          status = ca_get_callback
            (DBR_CTRL_DOUBLE, cd->chan_id, info_callback, curve);
          if (status != ECA_NORMAL)
          {
            SEVCHK
              (status, "StripDAQ connect_callback: error in ca_get_callback");
            Strip_freecurve (cd->this->strip, curve);
          }
        }
        break;
      
      case cs_closed:
        fprintf (stderr, "StripDAQ connect_callback: invalid chid\n");
        break;
  }

  fflush (stderr);

  ca_flush_io();
}


/*
 * info_callback
 */
static void     info_callback           (struct event_handler_args args)
{
  StripCurve                    curve;
  struct _ChannelData           *cd;
  struct dbr_ctrl_double        *ctrl;
  int                           status;
  double                        low, hi;

  curve = (StripCurve)(ca_puser (args.chid));
  cd = (struct _ChannelData *)StripCurve_getattr_val
    (curve, STRIPCURVE_FUNCDATA);

  if (args.status != ECA_NORMAL)
  {
    fprintf
      (stderr,
       "StripDAQ info_callback:\n"
       "  [%s] get: %s\n",
       ca_name(cd->chan_id),
       ca_message_text[CA_EXTRACT_MSG_NO(args.status)]);
  }
  else
  {
    ctrl = (struct dbr_ctrl_double *)args.dbr;

    low = ctrl->lower_disp_limit;
    hi = ctrl->upper_disp_limit;

    if (hi <= low)
    {
      low = ctrl->lower_ctrl_limit;
      hi = ctrl->upper_ctrl_limit;
      if (hi <= low)
      {
        if (ctrl->value == 0)
        {
          hi = 100;
          low = -hi;
        }
        else
        {
          low = ctrl->value - (ctrl->value / 10.0);
          hi = ctrl->value + (ctrl->value / 10.0);
        }
      }
    }

    if (!StripCurve_getstat (curve, STRIPCURVE_EGU_SET))
      StripCurve_setattr (curve, STRIPCURVE_EGU, ctrl->units, 0);
    if (!StripCurve_getstat (curve, STRIPCURVE_PRECISION_SET))
      StripCurve_setattr (curve, STRIPCURVE_PRECISION, ctrl->precision, 0);
    if (!StripCurve_getstat (curve, STRIPCURVE_MIN_SET))
      StripCurve_setattr (curve, STRIPCURVE_MIN, low, 0);
    if (!StripCurve_getstat (curve, STRIPCURVE_MAX_SET))
      StripCurve_setattr (curve, STRIPCURVE_MAX, hi, 0);

    status = ca_add_event
      (DBR_STS_DOUBLE, cd->chan_id, data_callback, curve, &cd->event_id);
    if (status != ECA_NORMAL)
    {
      SEVCHK
        (status, "StripDAQ info_callback: error in ca_get_callback");
      Strip_freecurve (cd->this->strip, curve);
    }
  }
  
  ca_flush_io();
}


/*
 * data_callback
 */
static void     data_callback           (struct event_handler_args args)
{
  StripCurve                    curve;
  struct _ChannelData           *cd;
  struct dbr_sts_double         *sts;

  curve = (StripCurve)ca_puser (args.chid);
  cd = (struct _ChannelData *)StripCurve_getattr_val
    (curve, STRIPCURVE_FUNCDATA);

  if (args.status != ECA_NORMAL)
  {
    fprintf
      (stderr,
       "StripDAQ data_callback:\n"
       "  [%s] get: %s\n",
       ca_name(cd->chan_id),
       ca_message_text[CA_EXTRACT_MSG_NO(args.status)]);
    Strip_setwaiting (cd->this->strip, curve);
  }
  else
  {
    if (StripCurve_getstat (curve, STRIPCURVE_WAITING))
    {
      StripCurve_setattr
        (curve, STRIPCURVE_SAMPLEFUNC, get_value, 0);
      Strip_setconnected (cd->this->strip, curve);
    }
    sts = (struct dbr_sts_double *)args.dbr;
    cd->value = sts->value;
  }
}


/*
 * get_value
 *
 *      Returns the current value specified by the CurveData passed in.
 */
static double   get_value       (void *data)
{
  struct _ChannelData   *cd = (struct _ChannelData *)data;

  return cd->value;
}
static void getDescriptionRecord(char *name, char *description)
{
  int status;
  chid id;
  static char desc_name[64];
  memset(desc_name,0,64);
  strcpy(desc_name,name);
  strcat(desc_name,".DESC");

  /* Check validity and initialize to blank */
  if(!description) {
#ifdef PRINT_DESC_ERRORS      
    fprintf(stderr,"Description array is NULL\n");
    return;
#endif    
  }
  *description='\0';
  
  status = ca_search(desc_name, &id);
  if (status != ECA_NORMAL) {
#ifdef PRINT_DESC_ERRORS      
    SEVCHK(status,"     Search for description field failed\n");
    fprintf(stderr,"%s: Search for description field failed\n",desc_name);
#endif    
    *description=0;
    return;      
  }
  
  status = ca_pend_io(1.0);	
  if (status != ECA_NORMAL) 
    {
#ifdef PRINT_DESC_ERRORS      
      SEVCHK(status,"     Search for description field timed out\n");
      fprintf(stderr,"%s: Search for description field timed out\n",desc_name);
      *description=0;
#endif    
      return;          
    }	
  
  status = ca_array_get (DBR_STRING,1,id,description);
  if (status != ECA_NORMAL) 
  {
#ifdef PRINT_DESC_ERRORS      
    SEVCHK(status,"     Get description field failed\n");
    fprintf(stderr,"%s: Get description field failed\n",desc_name);
    *description=0;
#endif    
    return;           
  }
  
  status= ca_pend_io(1.0);
  if (status != ECA_NORMAL)  
    {
#ifdef PRINT_DESC_ERRORS      
      SEVCHK(status,"     Get for description field timed out\n");
      fprintf(stderr,"%s: Get for description field timed out\n",name);
#endif    
      *description=0;
      return;     
    }

}

/* **************************** Emacs Editing Sequences ***************** */
/* Local Variables: */
/* tab-width: 6 */
/* c-basic-offset: 2 */
/* c-comment-only-line-offset: 0 */
/* c-indent-comments-syntactically-p: t */
/* c-label-minimum-indentation: 1 */
/* c-file-offsets: ((substatement-open . 0) (label . 2) */
/* (brace-entry-open . 0) (label .2) (arglist-intro . +) */
/* (arglist-cont-nonempty . c-lineup-arglist) ) */
/* End: */