#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>

/* 
 * This simple default synchronization mechanism allows only vehicle at a time
 * into the intersection.   The intersectionSem is used as a a lock.
 * We use a semaphore rather than a lock so that this code will work even
 * before locks are implemented.
 */

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */

/*
 * one lock and one condition variable
 */
static struct lock *mutex;
static struct cv *conflict;

//the max number of threads
static int NumThreads = 10;

typedef struct Vehicles
{
  Direction origin;
  Direction destination;
} Vehicle;

struct node {
    Vehicle vehicle;
    struct node *previous;
    struct node *next;
};

//vehicles is a queue
struct vehicles {
    struct node * volatile first;
    struct node * volatile last;
    volatile int total; 
};

static struct vehicles * Vqueue;

bool right_turn(Vehicle *v);
bool check_constraints(Vehicle *v);
void veh_free (struct vehicles *v);

//this function frees the queue
void veh_free (struct vehicles *v) {
    struct node * volatile current = v->first;
    if (current) {
        struct node * volatile next = current->next;
        kfree(current);
        current = next;
    }
    kfree(v);
}



/* 
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */
 
//check if the vehicle turns right
bool
right_turn(Vehicle *v) {
  KASSERT(v != NULL);
  if (((v->origin == west) && (v->destination == south)) ||
      ((v->origin == south) && (v->destination == east)) ||
      ((v->origin == east) && (v->destination == north)) ||
      ((v->origin == north) && (v->destination == west))) {
    return true;
  } else {
    return false;
  }
}

//check if the input vehicle can enter the intersection with the vehicles already in the queue
bool
check_constraints(Vehicle *v) {
  /* compare newly-added vehicle to each other vehicles in in the intersection */
  KASSERT(Vqueue->total <= NumThreads);
  struct node * volatile current = Vqueue->first;
  if ((current == NULL) && (Vqueue->total == 0)) {
        return true;
  }
    
  while(current) {
    /* no conflict if both vehicles have the same origin */
    if ((current->vehicle).origin == v->origin) {
        current = current->next;
        continue;
    }
    /* no conflict if vehicles go in opposite directions */
    if (((current->vehicle).origin == v->destination) &&
        ((current->vehicle).destination == v->origin)) {
        current = current->next;
        continue;
    }
    /* no conflict if one makes a right turn and 
       the other has a different destination */
    if ((right_turn(&(current->vehicle)) || right_turn(v)) &&
	(v->destination != (current->vehicle).destination)) {
        current = current->next;
        continue;
    }
    /* Houston, we have a problem! */
    return false;
  }
  return true;
}

 
 
 
void
intersection_sync_init(void)
{
  //initialize the lock and cv
  mutex = lock_create("traffic_lock");
  conflict = cv_create("traffic_conf");
  
  //initialize the vehicle queue
  Vqueue = kmalloc(sizeof(struct vehicles));
  Vqueue->first = NULL;
  Vqueue->last = NULL;
  Vqueue->total = 0;
  
  if (mutex == NULL) {
    panic("could not create the intersection lock");
  }
  
  if (conflict == NULL) {
    panic("could not create the intersection condition variable");
  }
  return;
}

/* 
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
void
intersection_sync_cleanup(void)
{
  KASSERT(mutex != NULL);
  KASSERT(conflict != NULL);
  KASSERT(Vqueue != NULL);
  
  //free the lock and queue
  lock_destroy(mutex);
  cv_destroy(conflict);
  //free the vehicle queue
  veh_free(Vqueue);
}


/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread 
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */

void
intersection_before_entry(Direction origin, Direction destination) 
{
  KASSERT(mutex != NULL);
  KASSERT(conflict != NULL);
  KASSERT(Vqueue != NULL);
  
  //modifying the queue, acquire the lock
  lock_acquire(mutex);
  
  Vehicle v;
  v.origin = origin;
  v.destination = destination;
  
  //check if the vehicle can enter the intersection, if not, wait until it can enter
  while (!check_constraints(&v)) {
      cv_wait(conflict, mutex);
  }
   
   //add the new vehicle to the queue
   struct node * volatile newNode = kmalloc(sizeof(struct node));
   newNode->vehicle = v;
   newNode->previous = Vqueue->last;
   newNode->next = NULL;
   if (Vqueue->last)
       Vqueue->last->next = newNode;
   else 
       Vqueue->first = newNode;
   Vqueue->last = newNode;
   Vqueue->total ++;
  
   lock_release(mutex);
  
}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

void
intersection_after_exit(Direction origin, Direction destination) 
{
  KASSERT(mutex != NULL);
  KASSERT(conflict != NULL);
  KASSERT(Vqueue != NULL);
  
  //modifying the queue, acquire the lock
  lock_acquire(mutex);
  KASSERT(Vqueue->total <= NumThreads);
  KASSERT(Vqueue->total > 0);
  struct node * volatile current = Vqueue->first;
  
  while (current) {
      //find the vehicle in Vqueue
      if (((current->vehicle).origin == origin) && ((current->vehicle).destination == destination)) {
          //remove the vehicle from the queue
          if (current->previous)
              current->previous->next = current->next;
          if (current->next)
              current->next->previous = current->previous;
          if (Vqueue->last == current)
              Vqueue->last = current->previous;
          if (Vqueue->first == current)
              Vqueue->first = current->next;
          kfree(current);
          Vqueue->total --;
          break;
      }
      current = current->next;
  }
  
  //wake up all the vehicles waiting
  cv_broadcast(conflict, mutex);
  lock_release(mutex);
}

