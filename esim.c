// gcc -o esim -O3 esim.c
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>


#define TMO_RANDOM -1
#define TMO_RANDOM_MIN 1
#define TMO_RANDOM_MAX 30
#define TMO_HBTIMEOUT 100
#define TMO_COUNTING (TMO_RANDOM_MAX*5)
#define TMO_BUS_IDLE (5*TMO_HBTIMEOUT)

#define STARTUP_MAX (TMO_BUS_IDLE*1)


#define EV_NONE 0
#define EV_BUS_IDLE 1
#define EV_HBTIMEOUT 2
#define EV_STARTUP 3
#define EV_SENSOR_DATA 4
#define EV_RANDOM 5
#define EV_COUNTING 6
/////////////////////
#define EV_HEARTBEAT 7
#define EV_WTF 8
#define EV_VOTING 9


#define NODE_COUNT 25
#define INVALID_ID 0
#define BROADCAST_ID 0
#define CLEAR_ALL 0

#define ST_READER 0
#define ST_VOTER 1
#define ST_LEADER 2

#define QMAX 100000


#define HASH(a) (a-1) // simulated bloom filter with k=1


const char *events[]=
{
  "NONE",
  "bus_idle",
  "hbtimeout",
  "startup",
  "sensor_data",
  "random",
  "counting",
  "heartbeat",
  "wtf",
  "voting",
};


struct node
{
  int id;
  int members[NODE_COUNT];
  int last_heartbeat_id;
  int state;
};

struct queue_item
{
  struct node *node;
  int ev;
  int param;
  int when;
  int invalid;
};

static struct node *current_node=NULL;
static int time=0;
static struct node nodes[NODE_COUNT];
static struct queue_item queue[QMAX];
static int queue_size=0;
static int processed_hb=0;
static int failrate=0;
static int maxdelay=1;
static int leader_change=0;
static int wtf_count=0;
static int seed=0;

#define queue_peektime() (queue_size>0?queue[0].when:0)

static struct queue_item *queue_next(void)
{
  static struct queue_item ret;
  
  if(queue_size>0)
  {
    memcpy(&ret,&queue[0],sizeof(struct queue_item));
    memmove(&queue[0],&queue[1],sizeof(struct queue_item)*queue_size);
    queue_size--;
    return(&ret);
  }

  return(NULL);
}


static int queue_add(struct node *n, int e, int p, int t)
{
  int w,i,to=-1;
  
  w=time+t;
  for(i=0;i<queue_size;i++)
  {
    if(w<queue[i].when)
    {
      to=i;
      break;
    }
  }
  if(to==-1) to=queue_size;
  memmove(&queue[to+1],&queue[to],sizeof(struct queue_item)*(queue_size));
  queue[to].node=n;
  queue[to].ev=e;
  queue[to].param=p;
  queue[to].when=w;
  queue[to].invalid=0;
  queue_size++;
  
  return(queue_size);
}


static int print_queue(void)
{
  int i;
  for(i=0;i<queue_size;i++) printf("Q %d @%d for %d [%s] %s\n",i,queue[i].when,queue[i].node->id,events[queue[i].ev],queue[i].invalid==0?"T":"F");
}


static int f_timer(int timer, int timeout)
{
  //printf("setting timer %s for %d to %d\n",events[timer],current_node->id,timeout);
  if(timeout==TMO_RANDOM) timeout=(rand()%(TMO_RANDOM_MAX-TMO_RANDOM_MIN))+TMO_RANDOM_MIN;
  queue_add(current_node,timer,0,timeout);
  //print_queue();
  return(0);
}

static void f_timer_clear(int ev)
{
  int i;
  //printf("clear timers for %d ev %d\n",current_node->id,ev);
  for(i=0;i<queue_size;i++) if(queue[i].node==current_node&&(ev==0||ev==queue[i].ev)) queue[i].invalid=1;
}

static int f_send(int dest, int ev, int param)
{
  int i;
  //printf("sending %s to %d param=%d\n",events[ev],dest,param);
  if(dest==BROADCAST_ID) for(i=0;i<NODE_COUNT;i++) queue_add(&nodes[i],ev,param,1+(rand()%maxdelay));
  else queue_add(&nodes[dest-1],ev,param,1+(rand()%maxdelay));
  return(0);
}

int event(struct node *node, int ev, int param)
{
  int ret=-1;
  
  current_node=node;
  if(NULL!=node&&ev>0)
  {
    ret=0;
    //printf("%d. %d %s(%d) [q=%d]\n",time,node->id,events[ev],param,queue_size);
    
    if(node->state==ST_READER) ////////////////////////////////	READER ///////////////////////
    {
      if(ev==EV_STARTUP)
      {
        f_timer_clear(CLEAR_ALL);
        f_timer(EV_BUS_IDLE,TMO_BUS_IDLE);
        //printf("  startup!\n");
      }
      else if(ev==EV_BUS_IDLE)                               // tmo BUS_IDLE -----------------
      {
        f_timer_clear(CLEAR_ALL);                            //   switch to VOTER
        f_timer(EV_COUNTING,TMO_COUNTING);                   //   reset timer COUNTING
        f_timer(EV_RANDOM,TMO_RANDOM);                       //   reset timer RANDOM
        bzero(&node->members,sizeof(node->members));         //   init members array/hash/queue
        node->state=ST_VOTER;
        //printf("  switching to VOTER\n");
      }
      else if(ev==EV_HEARTBEAT)                              // msg HEARTBEAT ----------------
      {
        if(param<=node->last_heartbeat_id
           ||node->last_heartbeat_id==INVALID_ID)            //   if(hb.id <= last_id)
        { 
          node->last_heartbeat_id=param;                     //     last_id = hb.id
          f_timer_clear(EV_BUS_IDLE);
          f_timer(EV_BUS_IDLE,TMO_BUS_IDLE);                 //     reset timer BUS_IDLE
          processed_hb++;
        }
      }
      else if(ev==EV_SENSOR_DATA)                            // msg SENSOR_DATA --------------
      {
        f_send(node->last_heartbeat_id,EV_SENSOR_DATA,0);    //   forward data to last_id
      }
      else ret=-1;
    }
    else if(node->state==ST_VOTER) ////////////////////////// VOTER //////////////////////////
    {
      if(ev==EV_COUNTING)                                    // tmo COUNTING -----------------
      {
        int count,i,min;
        for(min=INVALID_ID,count=i=0;                        //   count the members
            count<=1&&i<NODE_COUNT;i++)
        {
          if(node->members[i]!=0&&++count==1) min=i+1;       //   select the min pri
        }
        if(min==node->id&&count>1)                           //   if min==self and count>1
        {
          node->state=ST_LEADER;                             //     switch to LEADER
          f_timer_clear(CLEAR_ALL);
          f_timer(EV_HBTIMEOUT,TMO_HBTIMEOUT);               //       reset timer HEARTBEAT
          processed_hb=0;
          leader_change++;
          //printf("  switching to LEADER\n");
        }
        else                                                 //   else
        {
          f_timer_clear(CLEAR_ALL);                          //     switch to READER
          f_timer(EV_BUS_IDLE,TMO_BUS_IDLE);                 //       reset timer BUS_IDLE
          node->state=ST_READER;
          node->last_heartbeat_id=INVALID_ID;                //       clear last_id
          //printf("  switching to READER\n");
        }
      }
      else if(ev==EV_RANDOM)                                 // tmo RANDOM -------------------
      {
        f_send(BROADCAST_ID,EV_VOTING,node->id);             //   broadcast VOTING (self)
        f_timer(EV_RANDOM,TMO_RANDOM);                       //   reset timer RANDOM
      }
      else if(ev==EV_HEARTBEAT)                              // msg HEARTBEAT ----------------
      {
        f_timer_clear(CLEAR_ALL);                            //   switch to READER
        f_timer(EV_BUS_IDLE,TMO_BUS_IDLE);                   //     reset timer BUS_IDLE
        node->state=ST_READER;
        node->last_heartbeat_id=INVALID_ID;                  //     clear last_id
        //printf("  switching to READER\n");
      }
      else if(ev==EV_SENSOR_DATA)                            // msg SENSOR_DATA --------------
      {
        ;//printf("  processing sensor_data\n");             //   process data
      }
      else if(ev==EV_VOTING)                                 // msg VOTING -------------------
      {
        if((++node->members[HASH(param)])==1)                //   if id not seen:
        {
          f_timer_clear(EV_COUNTING);
          f_timer(EV_COUNTING,TMO_COUNTING);                 //     reset timer COUNTING
        }
      }
      else ret=-1;
    }
    else if(node->state==ST_LEADER) ///////////////////////// LEADER /////////////////////////
    {
      if(ev==EV_HBTIMEOUT)                                 // tmo HEARTBEAT ------------------
      {
        f_send(BROADCAST_ID,EV_HEARTBEAT,node->id);        //   broadcast HEARTBEAT
        f_timer(EV_HBTIMEOUT,TMO_HBTIMEOUT);               //   reset timer HEARTBEAT
        //printf(  "next hb %d\n",time+TMO_HBTIMEOUT);
      }
      else if(ev==EV_HEARTBEAT)                            // msg HEARTBEAT ------------------
      {
        if(param>node->id) f_send(param,EV_WTF,0);         //   if sender.pri < pri: send WTF
      }
      else if(ev==EV_SENSOR_DATA)                          // msg SENSOR_DATA ----------------
      {
        ; //printf("  processing sensor_data\n");          //   process data
      }
      else if(ev==EV_WTF)                                  // msg WTF ------------------------
      {
        f_timer_clear(CLEAR_ALL);                          //   switch to READER
        f_timer(EV_BUS_IDLE,TMO_BUS_IDLE);                 //     reset timer BUS_IDLE
        node->state=ST_READER;
        node->last_heartbeat_id=INVALID_ID;                //     clear last_id
        wtf_count++;
        //printf("  switching to READER\n");
      }
      else ret=-1;
    }
  }
  
  return(ret);
}


int main(int argc, char **argv)
{
  int i,j,n,max=-1,min=-1,sbase;
  int lc_total=0,lc_min=-1,lc_max=-1;
  long sum=0;
  struct queue_item *q;
  
  if(argc!=5)
  {
    printf("Usage: %s seed-base N fail_rate max_delay\n",argv[0]);
    exit(0);
  }
  sbase=atoi(argv[1]);
  n=atoi(argv[2]);
  failrate=atoi(argv[3]);
  maxdelay=atoi(argv[4]);
  wtf_count=0;
  for(j=0;j<n;j++)
  {
    seed=sbase+j;
    srand(seed);
    time=0;
    processed_hb=0;
    leader_change=0;
    current_node=NULL;
    bzero(nodes,sizeof(struct node)*NODE_COUNT);
    for(i=0;i<NODE_COUNT;i++) nodes[i].id=i+1;
    queue_size=0;
    for(i=0;i<NODE_COUNT;i++) queue_add(&nodes[i],EV_STARTUP,0,1+(rand()%STARTUP_MAX));
    while(processed_hb<(10*NODE_COUNT))
    {
      q=NULL;
      while(queue_peektime()==time)
      {
        q=queue_next();
        if(NULL!=q&&q->invalid==0&&( q->ev<EV_HEARTBEAT||(rand()%100)>failrate) ) event(q->node,q->ev,q->param);
        //print_queue();
      }
      if(q==NULL&&queue_peektime()!=0) time=queue_peektime();
    }
    sum+=time;
    if(time>max) max=time;
    if(min<0||time<min) min=time;
    lc_total+=leader_change;
    if(leader_change>lc_max) lc_max=leader_change;
    if(lc_min<0||leader_change<lc_min) lc_min=leader_change;
    //printf("%d. %d\n",j,time);
  }
  printf("done (*10 ms)\ntotal=%ld secs avg=%ld max=%d min=%d lc_ave=%d lc_max=%d lc_min=%d wtf=%d\n",sum/100,sum/n,max,min,lc_total/n,lc_max,lc_min,wtf_count);
  
  return(0);
}
