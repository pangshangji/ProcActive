
#include "list.h"

#ifndef NULL
#define NULL (void *)0
#endif

//---------------------------------------------------------------------------
// List_Init
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// List_Insert_Before
//---------------------------------------------------------------------------

void List_Insert_Before(LIST *list, void *oldelem, void *newelem)
{
    LIST_ELEM *prev, *curr, *next, *nelm;

    curr = (LIST_ELEM *)oldelem;
    nelm = (LIST_ELEM *)newelem;
    ++list->count;

    if (curr == list->head || curr == NULL) {     /* Insert head elem */
      next = list->head;
      list->head = nelm;
      nelm->prev = NULL;
      nelm->next = next;
      if (next == NULL)                         /* Old head is NULL? */
         list->tail = nelm;
      else
         next->prev = nelm;

    } else {                                  /* Insert non-head elem */
      prev = curr->prev;
      prev->next = nelm;
      nelm->prev = prev;
      curr->prev = nelm;
      nelm->next = curr;
    }
}

//---------------------------------------------------------------------------
// List_Insert_After
//---------------------------------------------------------------------------

void List_Insert_After(LIST *list, void *elem, void *newelem)
{
    LIST_ELEM *prev, *curr, *next, *nelm;

    curr = (LIST_ELEM *)elem;
    nelm = (LIST_ELEM *)newelem;
    ++list->count;

    if (curr == list->tail || curr == NULL) {     /* Insert tail elem */
      prev = list->tail;
      list->tail = nelm;
      nelm->prev = prev;
      nelm->next = NULL;
      if (prev == NULL)                         /* Old tail is NULL? */
         list->head = nelm;
      else
         prev->next = nelm;

    } else {                                  /* Insert non-tail elem */
      next = curr->next;
      next->prev = nelm;
      nelm->next = next;
      curr->next = nelm;
      nelm->prev = curr;
    }
}

//---------------------------------------------------------------------------
// List_Remove
//---------------------------------------------------------------------------

void List_Remove(LIST *list, void *elem)
{
    LIST_ELEM *prev, *curr, *next;

    curr = (LIST_ELEM *)elem;
    prev = curr->prev;
    next = curr->next;
    --list->count;

    if (prev == NULL && next == NULL) {         /* Last elem in list? */
        list->tail = NULL;
        list->head = NULL;

    } else if (prev == NULL) {                       /* Elem is head? */
        list->head = next;
        next->prev = NULL;

    } else if (next == NULL) {                       /* Elem is tail? */
        list->tail = prev;
        prev->next = NULL;

    } else {
        prev->next = next;              /* Elem is non-head, non-tail */
        next->prev = prev;
    }
}

