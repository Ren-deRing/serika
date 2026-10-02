#pragma once

#include <stddef.h>
#include <stdbool.h>

typedef struct list_node {
    struct list_node *next;
    struct list_node *prev;
} list_node;

#define LIST_HEAD_INIT(name) { &(name), &(name) }

static inline void list_init(list_node *head) {
    head->next = head;
    head->prev = head;
}

static inline void __list_add(list_node *new_node, list_node *prev, list_node *next) {
    next->prev = new_node;
    new_node->next = next;
    new_node->prev = prev;
    prev->next = new_node;
}

static inline void list_add(list_node *new_node, list_node *head) {
    __list_add(new_node, head, head->next);
}

static inline void list_add_tail(list_node *new_node, list_node *head) {
    __list_add(new_node, head->prev, head);
}   

static inline void __list_del(list_node *prev, list_node *next) {
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(list_node *node) {
    __list_del(node->prev, node->next);
    node->next = NULL;
    node->prev = NULL;
}

static inline bool list_empty(const list_node *head) {
    return head->next == head;
}

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_first(head, type, member) \
    container_of((head)->next, type, member)
