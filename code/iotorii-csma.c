

//#include "net/mac/csma/csma.h"
//#include "net/mac/csma/csma-output.h"
#include "csma-output.h"
#include "iotorii-csma.h"
#include "sys/ctimer.h"
#include "lib/list.h"
#include <stdlib.h> //For malloc()
#include "lib/random.h"
#include "hlmac-table.h"

#if LOG_DBG_STATISTIC == 1
#include "sys/node-id.h" //For node_id
#include "sys/rtimer.h" //For rtimer_clock_t, RTIMER_NOW()
#endif

#include "net/mac/mac-sequence.h"
#include "net/packetbuf.h"
#include "net/netstack.h"

/* Log configuration */

#include "sys/log.h"
#if IOTORII_NODE_TYPE == 0 //The traditional MAC operation
#define LOG_MODULE "CSMA"
#elif IOTORII_NODE_TYPE > 0 // 1 => the root node, 2 => the common nodes
#define LOG_MODULE "IoTorii-CSMA"
#endif
#define LOG_LEVEL LOG_LEVEL_MAC

/*---------------------------------------------------------------------------*/

//DELAY DESDE QUE SE INICIALIZA UN NODO HASTA QUE SE ENVÍAN LOS MENSAJES HELLO A LOS VECINOS
//RANGO = [IOTORII_CONF_HELLO_START_TIME/2 IOTORII_CONF_HELLO_START_TIME]
#ifdef IOTORII_CONF_HELLO_START_TIME
#define IOTORII_HELLO_START_TIME IOTORII_CONF_HELLO_START_TIME
#else
#define IOTORII_HELLO_START_TIME 4 //Default Delay is 2 s
#endif

//DELAY DESDE QUE SE INICIALIZA EL NODO ROOT HASTA QUE SE ENVÍA EL PRIMER MENSAJE SETHLMAC A LOS VECINOS
//SE IMPRIMER LOS PRIMEROS LOGS DE ESTADÍSTICAS
#ifdef IOTORII_CONF_SETHLMAC_START_TIME
#define IOTORII_SETHLMAC_START_TIME IOTORII_CONF_SETHLMAC_START_TIME
#else
#define IOTORII_SETHLMAC_START_TIME 5 //Default Delay is 10 s
#endif

//DELAY DESDE QUE SE INICIALIZA UN NODO COMÚN HASTA QUE SE ENVÍA MENSAJE SETHLMAC A LOS VECINOS
//RANGO = [IOTORII_CONF_SETHLMAC_DELAY/2 IOTORII_CONF_SETHLMAC_DELAY]
#ifdef IOTORII_CONF_SETHLMAC_DELAY
#define IOTORII_SETHLMAC_DELAY IOTORII_CONF_SETHLMAC_DELAY
#else
#define IOTORII_SETHLMAC_DELAY 5
#endif

//DELAY DE PRIMERAS STADÍSTICAS
#ifdef IOTORII_CONF_STATISTICS1_TIME
#define IOTORII_STATISTICS1_TIME IOTORII_CONF_STATISTICS1_TIME
#else
#define IOTORII_STATISTICS1_TIME 8
#endif

//DELAY DE LAS DEMÁS STADÍSTICAS
#ifdef IOTORII_CONF_STATISTICS2_TIME
#define IOTORII_STATISTICS2_TIME IOTORII_CONF_STATISTICS2_TIME
#else
#define IOTORII_STATISTICS2_TIME 2 
#endif

//DELAY DE INFORMACIÓN DE LA CARGA QUE TIENE UN NODO A SUS VECINOS
#ifdef IOTORII_CONF_LOAD_START_TIME
#define IOTORII_LOAD_START_TIME IOTORII_CONF_LOAD_START_TIME
#else
#define IOTORII_LOAD_START_TIME 3
#endif

//DELAY DE TRASPASOS DE CARGAS
#ifdef IOTORII_CONF_SHARE_START_TIME
#define IOTORII_SHARE_START_TIME IOTORII_CONF_SHARE_START_TIME
#else
#define IOTORII_SHARE_START_TIME 8 //10 POR DEFECTO -> AUMENTAR EN CASO DE MAS DE 30 NODOS
#endif

/*CUANDO TERMINA EL PRIMER PASO DE ENVÍO DE CARGAS (NODOS EDGE) COMIENZA EL SHARE TIMER PARA LOS NODOS EDGE.
PARA QUE NO INTERFIERA CON EL SEGUNDO PASO DE ENVÍOS DE CARGA (NODOS NO EDGE) EL SHARE_TIME > 2*LOAD_TIME*/

/*---------------------------------------------------------------------------*/

#if IOTORII_NODE_TYPE == 1 //ROOT
static struct ctimer sethlmac_timer;
#endif

#if IOTORII_NODE_TYPE > 0 //ROOT O NODO COMÚN
static struct ctimer hello_timer;
static struct ctimer send_sethlmac_timer;
static struct ctimer load_timer;
static struct ctimer share_timer;

#if LOG_DBG_STATISTIC == 1
static struct ctimer statistic_timer;
int number_of_hello_messages = 0;
int number_of_sethlmac_messages = 0;

int number_of_load_edge_messages = 0;
int number_of_load_no_edge_messages = 0;
#endif

#endif

#if IOTORII_NODE_TYPE > 0 //ROOT O NODO COMÚN
LIST(neighbour_table_entry_list); //SE CREA LA TABLA DEL VECINO

struct payload_entry //ESTRUCTURA DE ENTRADA DE TABLA PARA DIRECCIONES DE RECEPTORES O PAYLOAD
{
	struct payload_entry *next;
	uint8_t *payload;
	int data_len;
};

typedef struct payload_entry payload_entry_t;
LIST(payload_entry_list);

/*---------------------------------------------------------------------------*/

struct this_node //ESTRUCTURA DE ENTRADA DE TABLA DE PRIORIDADES Y DE RELACIONES PADRE-HIJO
{
	struct this_node *next;
	char* str_addr;
	char* str_top_addr; //ADDR PADRE
	uint8_t payload;
	uint8_t nhops;
	int load; //CARGA DE CADA NODO -> 16 BITS PORQUE CON 8 BITS A PARTIR DE 255 DE CARGA SE VUELVE 0
	//int load_to_next; //CARGA QUE SE QUEDA ALMACENADA PARA ENVIAR AL HIJO EN LA SEGUNDA VUELTA DEL REPARTO
};

typedef struct this_node this_node_t;
LIST(node_list); //aux

void list_this_node_entry (payload_entry_t *a, hlmacaddr_t *addr); //GUARDA INFO EN this_node

uint8_t n_hijos = 0; //CUENTA DEL NÚMERO DE NODOS HIJOS
uint8_t count_hijos = 0; //CONTROL PARA QUE HAYA UHA ÚNICA CUENTA DE LOS HIJOS

uint8_t edge = 0; //INDICA QUE EL NODO ES EDGE SI ES 1
uint8_t first_edge_sent = 0; //INDICA QUE SE HA ENVIADO EL PRIMER MENSAJE EDGE SI ES 1

uint8_t start_load = 0;
uint8_t sent_no_edge = 0; //INDICA A 1 LA ACTUALIZACIÓN COMPLETA DE LA LISTA DE CARGAS DE LOS VECINOS DE TODOS LOS NODOS (EDGE + NO EDGE)

uint8_t start_share = 0;
uint8_t msg_share_on = 0;
uint8_t new_edge = 0; //MARCA COMO EDGE UN NODO NO EDGE QUE YA HA REPARTIDO SU CARGA

int extra_load = 0; //INDICA CUANTA CARGA SOBRA SI UN NODO TIENE UNA CANTIDAD MAYOR A 100 
int new_extra_load = 0; //INDICA CUANTA CARGA DEL TOTAL SOBRANTE SE ENVÍA AL PADRE

uint8_t edge_complete_load = 0; //INDICA A 1 QUE SE ACTIVA EN LA SEGUNDA VUELTA LA RECEPCIÓN DE CARGA EN LOS EDGE PARA CONSEGUIR CARGA = 100

#endif

/*---------------------------------------------------------------------------*/

static void iotorii_handle_statistic_timer ();

static void init_sec (void)
{
#if LLSEC802154_USES_AUX_HEADER
    if (packetbuf_attr(PACKETBUF_ATTR_SECURITY_LEVEL) == PACKETBUF_ATTR_SECURITY_LEVEL_DEFAULT) 
		packetbuf_set_attr(PACKETBUF_ATTR_SECURITY_LEVEL, CSMA_LLSEC_SECURITY_LEVEL);
#endif
}


static void send_packet (mac_callback_t sent, void *ptr) //ENVÍA PAQUETE
{
	init_sec();
	csma_output_packet(sent, ptr);
}


static int on (void)
{
    return NETSTACK_RADIO.on();
}


static int off (void)
{
    return NETSTACK_RADIO.off();
}


static int max_payload (void)
{
	int framer_hdrlen;

	init_sec();

	framer_hdrlen = NETSTACK_FRAMER.length();

	if (framer_hdrlen < 0) //FRAMING HA FALLADO Y SE ASUME LA LONGITUD MAX DE CABECERA
		framer_hdrlen = CSMA_MAC_MAX_HEADER;

	return CSMA_MAC_LEN - framer_hdrlen;
}

/*---------------------------------------------------------------------------*/

#if IOTORII_NODE_TYPE > 0 //ROOT O NODO COMÚN

hlmacaddr_t *iotorii_extract_address (void) //SE EXTRAE LA DIRECCIÓN DEL NODO EMISOR A PARTIR DEL PAQUETE RECIBIDO
{
	int packetbuf_data_len = packetbuf_datalen();
	uint8_t *packetbuf_ptr_head = (uint8_t*) malloc (sizeof(uint8_t) * packetbuf_data_len);
	memcpy(packetbuf_ptr_head, packetbuf_dataptr(), packetbuf_data_len); //COPIA DEL BUFFER
	
	uint8_t *packetbuf_ptr = packetbuf_ptr_head; //PUNTERO A COPIA DEL BUFFER
	int datalen_counter = 0;
	//uint8_t pref_len;
	
	uint8_t id = 0;
	hlmacaddr_t *prefix = NULL;
	linkaddr_t link_address = linkaddr_null;
	uint8_t is_first_record = 1;  

	//SE LEE EL PAYLOAD
	
	/* The payload structure:
	* +------------+--------+-----+------+-----+-----+------+
	* | Prefix len | Prefix | ID1 | MAC1 | ... | IDn | MACn |
	* +------------+--------+-----+------+-----+-----+------+ -------->*/
	
	while (datalen_counter < packetbuf_data_len && !linkaddr_cmp(&link_address, &linkaddr_node_addr))
	{
		if (is_first_record) //SI ES EL PRIMER RECORD DEL PAYLOAD SE INCLUYE EL PREFIJO Y LA LONGITUD DE ESTE
		{
			is_first_record = 0;
			
			prefix = (hlmacaddr_t*) malloc (sizeof(hlmacaddr_t)); //SE RESERVA MEMORIA PARA EL PREFIJO
			memcpy(&prefix->len, packetbuf_ptr, 1); //COPIA LONGITUD PREFIJO DEL VECINO
			
			packetbuf_ptr++;
			datalen_counter++;

			prefix->address = (uint8_t*) malloc (sizeof(uint8_t) * prefix->len); //COPIA PREFIJO DEL VECINO
			memcpy(prefix->address, packetbuf_ptr, prefix->len);
			
			packetbuf_ptr += prefix->len;
			datalen_counter += prefix->len;
		}
		
		memcpy(&id, packetbuf_ptr, 1); //COPIA ID DEL VECINO
		
		packetbuf_ptr++;
		datalen_counter++;

		memcpy(&link_address, packetbuf_ptr, LINKADDR_SIZE); //COPIA DIRECCIÓN MAC DEL VECINO
		
		packetbuf_ptr += LINKADDR_SIZE;
		datalen_counter += LINKADDR_SIZE;
	} 
	
	free(packetbuf_ptr_head); //SE LIBERA MEMORIA
	packetbuf_ptr_head = NULL;
	
	if (linkaddr_cmp(&link_address, &linkaddr_node_addr))
	{
		hlmac_add_new_id(prefix, id); //SE AÑADE ID A LA DIRECCIÓN DEL PREFIJO
		return prefix;  
	}
	
	*prefix = UNSPECIFIED_HLMAC_ADDRESS; //SE "BORRA"
	return prefix;
}


void iotorii_handle_load_timer ()
{	
	this_node_t *node;
	node = list_head(node_list); 
	
	if (list_head(node_list)) //EXISTE 
	{
		packetbuf_clear(); //SE PREPARA EL BUFFER DE PAQUETES Y SE RESETEA 
		
		memcpy(packetbuf_dataptr(), &(node->load), sizeof(&node->load)); //SE COPIA LOAD  
		packetbuf_set_datalen(sizeof(&node->load));									
		packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, &linkaddr_null);

		if (edge == 1 && first_edge_sent == 0)
			first_edge_sent = 1;

		#if LOG_DBG_STATISTIC == 1		
		printf("//INFO HANDLE LOAD// carga enviada: %d de direccion %s\n", node->load, node->str_addr); 
		#endif

		send_packet(NULL, NULL);
	}
}


void iotorii_handle_share_upstream_timer ()
{
	this_node_t *node;
	node = list_head(node_list); 
	
	neighbour_table_entry_t *nb;
	nb = list_head(neighbour_table_entry_list);
	
	msg_share_on = 1;
	//uint8_t one_time = 0; //VARIABLE UTILIZADA CUANDO EXISTEN DOS HIJOS QUE PUEDEN ABSORBER CARGA, PARA QUE SOLO ELIJA A 1
	//uint8_t n_hijos_to_send = 0; //CUENTA DEL NÚMERO DE NODOS HIJOS CON CARGA < 100
	//uint8_t n_hijos_to_send_ready = 0;
	
	//FLUJOS DE TRÁFICO UPWARD: DESDE LOS EDGE HACIA EL DODAG (NODO ROOT)
	//TODOS LOS NODOS SE QUEDAN CON UNA CARGA DE 100 
	if ((edge == 1 || new_edge == 1) && start_share == 0)
	{		
		if (node->load > 100)
		{
			extra_load = node->load - 100; //SE QUITA LA CARGA SOBRANTE Y SE ALMACENA 			
			printf("//INFO HANDLE SHARE UP// En este nodo sobra una carga de %d\n", extra_load);		
		}
		
		else //node->load <= 100
		{
			if (node->load > 0)
				extra_load = node->load - 100; //SE QUITA LA CARGA SOBRANTE Y SE ALMACENA 
			else
			{
				extra_load = -node->load + 100;
				extra_load = -extra_load;
			}
			
			printf("//INFO HANDLE SHARE UP// En este nodo permite una carga adicional de valor %d\n", -extra_load);
		}
		
		#if IOTORII_NODE_TYPE == 2
		node->load = 100; //SE ASIGNA COMO MÁXIMO UNA CANTIDAD DE 100 PARA NODOS COMUNES
		#endif
		
		for (nb = list_head(neighbour_table_entry_list); nb != NULL; nb = list_item_next(nb))
		{
			if (nb->flag == 1)
			{
				nb->in_out = -extra_load;
				packetbuf_clear(); //SE PREPARA EL BUFFER DE PAQUETES Y SE RESETEA 
				
				memcpy(packetbuf_dataptr(), &(extra_load), sizeof(extra_load)); //SE COPIA LOAD  
				packetbuf_set_datalen(sizeof(extra_load));									
				packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, &linkaddr_null);
			
				printf("//INFO HANDLE SHARE UP// Carga actualizada: %d\n", node->load);
				printf("//INFO HANDLE SHARE UP// Carga informada: %d\n", extra_load); 
			}	
		}
		
		send_packet(NULL, NULL);
	}
	
	start_share = 1; // ANTES
	ctimer_set(&statistic_timer, IOTORII_STATISTICS2_TIME * CLOCK_SECOND, iotorii_handle_statistic_timer, NULL); //SE MOSTRARÁN LAS ESTADÍSTICAS ACTUALIZADAS
}


#if LOG_DBG_STATISTIC == 1

static void iotorii_handle_statistic_timer ()
{
	uint8_t load_null = 0;
	
	this_node_t *node;
	node = list_head(node_list); 
	neighbour_table_entry_t *nb;
	
	if (start_share == 0 && msg_share_on == 0)
	{
		//printf("Periodic Statistics: node_id: %u, n_hello: %d, n_sethlmac: %d, n_neighbours: %d\n", node_id, number_of_hello_messages, number_of_sethlmac_messages, number_of_neighbours);
		printf("//INFO STATISTICS// n_hello: %d, n_sethlmac: %d\n", number_of_hello_messages, number_of_sethlmac_messages);
		printf("//INFO STATISTICS// El nodo %s tiene %d vecinos y no ha recibido HLMAC de %d vecinos: ", node->str_addr, number_of_neighbours, number_of_neighbours_flag);
		
		if (!number_of_neighbours_flag)
		{
			printf("es edge\n");		
			edge = 1; 

			if (start_load == 0 && sent_no_edge == 0) //NO SE HA INICIADO EL ENVÍO DE MENSAJES TODAVÍA
				ctimer_set(&load_timer, IOTORII_LOAD_START_TIME * CLOCK_SECOND, iotorii_handle_load_timer, NULL); 
		}
		else
		{
			printf("no es edge\n");
			
			if (start_load == 1 && sent_no_edge == 0) //SEGUNDA VUELTA CUANDO YA SE HA INFORMADO DE LA CARGA DE LOS NODOS EDGE
			{
				ctimer_set(&load_timer, /*IOTORII_LOAD_START_TIME*/ (random_rand() % (IOTORII_LOAD_START_TIME*3)) * CLOCK_SECOND, iotorii_handle_load_timer, NULL); 
				sent_no_edge = 1;
			}
		}

		start_load = 1; //SE PONE A 1 CUANDO HAN EMPEZADO LOS EDGE A ENVIAR CARGAS Y ACTIVA LOS ENVÍOS EN LOS NODOS NO EDGE
		
		printf("Carga actual del nodo: %d\n", node->load);
		for (nb = list_head(neighbour_table_entry_list); nb != NULL; nb = list_item_next(nb)) //LISTA DE VECINOS DEL NODO
		{				
			if (nb->flag == 1 || nb->flag == -1)
				printf("--> Vecino padre (flag, carga, in_out) --> ");
			else
			{
				printf("--> Vecino hijo  (flag, carga, in_out) --> ");	
				if (count_hijos == 0)
					n_hijos++;
			}
			
			if (nb->flag == -1)
				printf("%d, %d, (%d)\n", nb->flag, nb->load, nb->in_out);		
			else
				printf("%d, %d, %d\n", nb->flag, nb->load, nb->in_out);		
		}
		
		count_hijos = 1; //SE HA REALIZADO LA CUENTA DE LOS HIJOS
		
		for (nb = list_head(neighbour_table_entry_list); nb != NULL; nb = list_item_next(nb))
		{
			if (nb->load == 0)
				load_null++; //SE CONTABILIZAN LOS VECINOS CON CARGA TODAVÍA DESCONOCIDA
		}		
	}
	else if (msg_share_on == 1)
	{
		printf("Carga actual del nodo: %d\n", node->load);
		for (nb = list_head(neighbour_table_entry_list); nb != NULL; nb = list_item_next(nb)) //LISTA DE VECINOS DEL NODO
		{
			if (nb->flag == 1 || nb->flag == -1)
				printf("--> Vecino padre (flag, carga inicial, in_out) --> ");
			else
				printf("--> Vecino hijo  (flag, carga inicial, in_out) --> ");
			
			if (nb->flag == -1)
				printf("%d, %d, (%d)\n", nb->flag, nb->load, nb->in_out);		
			else
				printf("%d, %d, %d\n", nb->flag, nb->load, nb->in_out);				
		}	
	}	
	
	if ((edge == 1 || new_edge == 1) && load_null == 0) //SI SE HAN ENVIADO TODOS LOS MENSAJES DE CARGA (PRIMERA ACTUALIZACIÓN COMPLETA)		
		ctimer_set(&share_timer, IOTORII_SHARE_START_TIME * CLOCK_SECOND, iotorii_handle_share_upstream_timer, NULL);	
	
	if (load_null == 0) //SI UN NODO SABE YA TODAS LAS CARGAS DE SUS VECINOS PUEDE COMENZAR CON EL REPARTO DE CARGAS
		msg_share_on = 1;
	else
		printf("//INFO STATISTICS// Faltan %d nodos por conocer su carga\n", load_null);
}

#endif


static void iotorii_handle_hello_timer ()
{
	int mac_max_payload = max_payload();
	
	if (mac_max_payload <= 0) //FRAMING HA FALLADO Y NO SE PUEDE CREAR HELLO
	   LOG_WARN("output: failed to calculate payload size - Hello can not be created\n");
	else
	{
		packetbuf_clear(); //HELLO NO TIENE PAYLOAD
		packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, &linkaddr_null);
		LOG_DBG("Hello prepared to send\n");
		printf("//INFO HANDLE HELLO// Mensaje Hello enviado\n");
		
		#if LOG_DBG_STATISTIC == 1
		number_of_hello_messages++; //SE INCREMENTA EL NÚMERO DE MENSAJES HELLO
		#endif

		send_packet(NULL, NULL);
	}
}


void iotorii_handle_send_sethlmac_timer ()
{
	payload_entry_t *payload_entry;
	payload_entry = list_pop(payload_entry_list); //POP PACKETBUF DE LA COLA
	
	if (payload_entry) //EXISTE LA ENTRADA PAYLOAD
	{
		//SE PREPARA EL BUFFER DE PAQUETES		
		packetbuf_clear(); //SE RESETEA EL BUFFER 
		
		memcpy(packetbuf_dataptr(), payload_entry->payload, payload_entry->data_len); //SE COPIA PAYLOAD
		packetbuf_set_datalen(payload_entry->data_len);

		//SE LIBERA MEMORIA
		free(payload_entry->payload);
		payload_entry->payload = NULL;
		free(payload_entry);
		payload_entry = NULL;

		//Control info: the destination address, the broadcast address, is tagged to the outbound packet
		packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, &linkaddr_null);
		LOG_DBG("Queue: SetHLMAC prepared to send\n");

		number_of_sethlmac_messages++; //SE INCREMENTA EL NÚMERO DE MENSAJES SETHLMAC
		
		if (list_head(payload_entry_list)) //SI LA LISTA TIENE UNA ENTRADA 
		{
			//SE PLANIFICA EL SIGUIENTE MENSAJE
			clock_time_t sethlmac_delay_time = IOTORII_SETHLMAC_DELAY/2 * (CLOCK_SECOND / 128);
			sethlmac_delay_time = sethlmac_delay_time + (random_rand() % sethlmac_delay_time);
			
			#if LOG_DBG_DEVELOPER == 1
			LOG_DBG("Scheduling a SetHLMAC message after %u ticks in the future\n", (unsigned)sethlmac_delay_time);
			#endif
			
			ctimer_set(&send_sethlmac_timer, sethlmac_delay_time, iotorii_handle_send_sethlmac_timer, NULL);
		}
		send_packet(NULL, NULL);
	}
	
	printf("INICIO CONVERGENCIA\n");
}


void iotorii_send_sethlmac (hlmacaddr_t addr, linkaddr_t sender_link_address)
{
	int mac_max_payload = max_payload();
	
	if (mac_max_payload <= 0) //FRAMING HA FALLADO Y SETHLMAC NO SE PUEDE CREAR
	   LOG_WARN("output: failed to calculate payload size - SetHLMAC can not be created\n");
	
	else
	{
		neighbour_table_entry_t *neighbour_entry;

		#if LOG_DBG_DEVELOPER == 1 
		LOG_DBG("Info before sending SetHLMAC: ");
		LOG_DBG("Number of neighbours: %d, mac_max_payload: %d, LINKADDR_SIZE: %d.\n", number_of_neighbours, mac_max_payload, LINKADDR_SIZE);
		#endif

		uint8_t number_of_neighbours_new = number_of_neighbours;

		for (neighbour_entry = list_head(neighbour_table_entry_list); neighbour_entry != NULL; neighbour_entry = list_item_next(neighbour_entry))
		{			
			if (linkaddr_cmp(&neighbour_entry->addr, &sender_link_address)) //SE BORRA EL NODO EMISOR DEL MENSAJE SETHLMAC RECIBIDO DEL NUEVO PAYLOAD
			{
				number_of_neighbours_new = number_of_neighbours - 1;
				
				#if LOG_DBG_DEVELOPER == 1
				LOG_DBG("Sender node is in neighbour list, number_of_neighbours: %u, number_of_neighbours_new: %u.\n", number_of_neighbours, number_of_neighbours_new);
				#endif
			}
		}

		if (number_of_neighbours_new > 0)
		{
			neighbour_table_entry_t **random_list = (neighbour_table_entry_t **) malloc(sizeof(neighbour_table_entry_t *) * number_of_neighbours_new);
			uint8_t j;
			unsigned short r;
			
			//SE CREA UNA SECUENCIA ALEATORIA PARA LOS VECINOS (ÚTIL SI EL PAYLOAD NO PUEDE ACOMODAR A TODOS LOS VECINOS)
			//TODOS LOS VECINOS TIENEN LA MISMA PRIORIDAD PARA SER ELEGIDOS
			
			for (j = 0; j < number_of_neighbours_new; j++)
			{
				random_list[j] = NULL;
			}
			
			for (neighbour_entry = list_head(neighbour_table_entry_list); neighbour_entry != NULL; neighbour_entry = list_item_next(neighbour_entry))
			{
				if (!linkaddr_cmp(&neighbour_entry->addr, &sender_link_address)) //EL NODO EMISOR NO ESTÁ EN LA LISTA DE VECINOS
				{
					while (random_list[r = random_rand() % (number_of_neighbours_new)] != NULL); //CADA VECINO TIENE UNA r ÚNICA
					random_list[r] = neighbour_entry;
				   
					#if LOG_DBG_DEVELOPER == 1
					LOG_DBG("number_of_neighbours_new = %u Neighbor (ID = %u) gets random priority %u\n", number_of_neighbours_new, random_list[r]->id, r);
					#endif
				}
				
				else //EL NODO EMISOR ESTÁ EN LA LISTA DE VECINOS
				{
					#if LOG_DBG_DEVELOPER == 1
					LOG_DBG("Sender node is eliminated in random list!\n");
					#endif
				}
		    }
 
			uint8_t *packetbuf_ptr_head;
			uint8_t *packetbuf_ptr;

			int datalen_counter = 0;
			uint8_t i = 1;

			//SE ACOMODAN LAS DIRECCIONES SETHLMAC EN EL PAYLOAD
			//SI EL NODO TIENE VECINOS PARA ENVIAR MENSAJE SETHLMAC Y EL PAYLOAD ES MEÑOR AL MÁXIMO PAYLOAD 
			if (random_list[i-1] && (mac_max_payload >= (addr.len + 2 + LINKADDR_SIZE))) //2 = LONGITUD DEL PREFIJO DE DIRECCIÓN HLMAC(1) + ID(1)
			{
				//SE CREA UNA COPIA DE PACKETBUF ANTES DE EMPEZAR EL PROCESO PARA QUE NO SE SOBREESCRIBA AL ENVIAR MENSAJES SETHLMAC
				packetbuf_ptr_head = (uint8_t *) malloc (sizeof(uint8_t) * mac_max_payload);
				packetbuf_ptr = packetbuf_ptr_head; //packetbuf_ptr = packetbuf_dataptr(); 
				
				/* The payload structure:
				* +------------+--------+-----+------+-----+-----+------+
				* | Prefix len | Prefix | ID1 | MAC1 | ... | IDn | MACn |
				* +------------+--------+-----+------+-----+-----+------+ -------->*/
				
				memcpy(packetbuf_ptr, &(addr.len), 1); //COPIA LONGITUD PREFIJO DE VECINO
				
				packetbuf_ptr++;
				datalen_counter++;
				
				memcpy(packetbuf_ptr, addr.address, addr.len); //COPIA PREFIJO DE VECINO
				
				packetbuf_ptr += addr.len;
				datalen_counter += addr.len;

				do
				{
					memcpy(packetbuf_ptr, &(random_list[i-1]->number_id), 1); //COPIA ID DE VECINO
					
					packetbuf_ptr++;
					datalen_counter++;
					
					memcpy(packetbuf_ptr, &(random_list[i-1]->addr), LINKADDR_SIZE); //COPIA DIRECCIÓN MAC DE VECINO
					
					packetbuf_ptr += LINKADDR_SIZE;
					datalen_counter += LINKADDR_SIZE;

					i++;
				} while (mac_max_payload >= (datalen_counter + LINKADDR_SIZE + 1) && i <= number_of_neighbours_new);

				//SE CREA Y SE ASIGNAN VALORES A LA ENTRADA DE PAYLOAD
				payload_entry_t *payload_entry = (payload_entry_t*) malloc (sizeof(payload_entry_t));
				payload_entry->next = NULL;
				payload_entry->payload = packetbuf_ptr_head;
				payload_entry->data_len = datalen_counter;

				
				if (!list_head(payload_entry_list)) //ANTES DE AÑADIR LA ENTRADA DE PAYLOAD A LA LISTA SE COMPRUEBA SI LA LISTA ESTA VACÍA PARA CONFIGURAR EL TIEMPO
				{
					clock_time_t sethlmac_delay_time = 0; //EL DELAY ANTES DE ENVIAR EL PRIMER MENSAJE SETHLMAC (ENVIADO POR ROOT) ES 0
					
					#if IOTORII_NODE_TYPE > 1 //SI NODO COMÚN SE PLANIFICAN DELAYS ANTES DE ENVIAR LOS DEMÁS MENSAJES SETHLMAC
					sethlmac_delay_time = IOTORII_SETHLMAC_DELAY/2 * (CLOCK_SECOND / 128);
					sethlmac_delay_time = sethlmac_delay_time + (random_rand() % sethlmac_delay_time);
					
					#if LOG_DBG_DEVELOPER == 1
					LOG_DBG("Scheduling a SetHLMAC message by the root node after %u ticks in the future\n", (unsigned)sethlmac_delay_time);
					#endif
					
					#endif
					
					list_add(payload_entry_list, payload_entry); //SE AÑADE AL FINAL DE LA LISTA LA ENTRADA DE PAYLOAD
					ctimer_set(&send_sethlmac_timer, sethlmac_delay_time, iotorii_handle_send_sethlmac_timer, NULL); //SET TIMER
				}
				else //SI NO ESTÁ VACÍA NO SE CONFIGURA EL TIEMPO Y DIRECTAMENTE SE AÑADE LA ENTRADA DE PAYLOAD
					list_add(payload_entry_list, payload_entry);
					
				list_this_node_entry (payload_entry, &addr); //RELLENA LA ESTRUCTURA	
				

				char *neighbour_hlmac_addr_str = hlmac_addr_to_str(addr);
				printf("SetHLMAC prefix (addr:%s) added to queue to advertise to %d nodes.\n", neighbour_hlmac_addr_str, i-1);
				LOG_DBG("SetHLMAC prefix (addr:%s) added to queue to advertise to %d nodes.\n", neighbour_hlmac_addr_str, i-1);
				free(neighbour_hlmac_addr_str);
			}			
			else //SI EL NODO NO TIENE VECINOS PARA ENVIAR MENSAJE SETHLMAC O EL PAYLOAD ES PEQUEÑO
				LOG_DBG("Node has not any neighbour (or payload is low) to send SetHLMAC.\n");


			//SE BORRA LA LISTA ALEATORIA DE VECINOS
			if (number_of_neighbours_new > 0) //SI SE HAN CREADO NUEVOS VECINOS
			{
				for (j = 0; j < number_of_neighbours_new; j++)
				{
					random_list[j] = NULL; 
				}
				
				free(random_list);
				random_list = NULL;
			}
		} //END if number_of_neighbours_new
	} // END else
}


void iotorii_handle_incoming_hello () //PROCESA UN PAQUETE HELLO (DE DIFUSIÓN) RECIBIDO DE OTROS NODOS
{
	const linkaddr_t *sender_addr = packetbuf_addr(PACKETBUF_ADDR_SENDER); //SE LEE EL BUFFER

	LOG_DBG("A Hello message received from ");
	LOG_DBG_LLADDR(sender_addr);
	LOG_DBG("\n");

	if (number_of_neighbours < 256) //SI NO SE HA SUPERADO EL MÁXIMO DE ENTRADAS EN LA LISTA
	{
		uint8_t address_is_in_table = 0;
		neighbour_table_entry_t *new_nb;
		
		for (new_nb = list_head(neighbour_table_entry_list); new_nb != NULL; new_nb = new_nb->next)
		{
			if (linkaddr_cmp(&(new_nb->addr), sender_addr))
				address_is_in_table = 1; //SE PONE A 1 SI LA DIRECCIÓN DEL EMISOR SE ENCUENTRA EN LA LISTA
		}
		
		if (!address_is_in_table) //SI NO ESTÁ EN LA LISTA
		{
			new_nb = (neighbour_table_entry_t*) malloc (sizeof(neighbour_table_entry_t)); //SE RESERVA ESPACIO
			new_nb->number_id = ++number_of_neighbours;
			number_of_neighbours_flag++;
			new_nb->addr = *sender_addr;
			new_nb->flag = 0;
			new_nb->load = 0; //CARGA INICIAL NULA
			new_nb->in_out = 0; //CARGA ENTRANTE O SALIENTE NULA AHORA
			
			list_add(neighbour_table_entry_list, new_nb); //SE AÑADE A LA LISTA
			
			LOG_DBG("A new neighbour added to IoTorii neighbour table, address: ");
			LOG_DBG_LLADDR(sender_addr);
			LOG_DBG(", ID: %d\n", new_nb->number_id);
			
			printf("//INFO INCOMING HELLO// Mensaje Hello recibido\n");
		}
		else
		{
			LOG_DBG("Address of hello (");
			LOG_DBG_LLADDR(sender_addr);
			LOG_DBG(") message received already!\n");
		}
	}
	else //TABLA LLENA (256)
		LOG_WARN("The IoTorii neighbour table is full! \n");	
}


void iotorii_handle_incoming_sethlmac_or_load () //PROCESA UN MENSAJE DE DIFUSIÓN SETHLMAC RECIBIDO DE OTROS NODOS
{
	LOG_DBG("A message received from ");
	LOG_DBG_LLADDR(packetbuf_addr(PACKETBUF_ADDR_SENDER));
	LOG_DBG("\n");

	hlmacaddr_t *received_hlmac_addr;
	received_hlmac_addr = iotorii_extract_address(); //SE COGE LA DIRECCIÓN RECIBIDA
	
	linkaddr_t sender_link_address = *packetbuf_addr(PACKETBUF_ADDR_SENDER);
	const linkaddr_t *sender = &sender_link_address;
	
	neighbour_table_entry_t *nb;	
	this_node_t *node;
	node = list_head(node_list);

	if (hlmac_is_unspecified_addr(*received_hlmac_addr)) //SI NO SE ESPECIFICA DIRECCIÓN, NO HAY PARA EL NODO
	{		
		if (start_load == 1 && msg_share_on == 0)
		{
			int packetbuf_data_len = packetbuf_datalen();
			
			uint8_t *p_load = (uint8_t *) malloc (sizeof(uint8_t));
			memcpy(p_load, packetbuf_dataptr(), packetbuf_data_len); //COPIA DEL BUFFER 
			
			char* str_sender = (char*) malloc (sizeof(char) * (LINKADDR_SIZE * (2 + 1) + 1));
			str_sender = link_addr_to_str (sender_link_address, 1);	//LENGTH = 1 PARA MOSTRAR SOLO EL ID
			
			for (nb = list_head(neighbour_table_entry_list); nb != NULL; nb = list_item_next(nb))
			{
				if (linkaddr_cmp(&nb->addr, sender)) //SE BUSCA EN LA LISTA DE VECINOS LA DIRECCIÓN QUE HA ENVIADO EL MENSAJE 
				{
					memcpy(&nb->load, packetbuf_dataptr(), packetbuf_data_len); //SE ACTUALIZA LA CARGA EN LA LISTA DE VECINOS						
					printf("//INFO INCOMING LOAD// carga recibida: %d del nodo 0x%s\n", *p_load, str_sender);
					ctimer_set(&statistic_timer, 0, iotorii_handle_statistic_timer, NULL); //SE MOSTRARÁN LAS ESTADÍSTICAS ACTUALIZADAS AUTOMÁTICAMENTE
				}
			}
			
			free(p_load);
			p_load = NULL;
			
			free(str_sender);
			str_sender = NULL;
		}
		
		else if (msg_share_on == 1)
		{
			int packetbuf_data_len = packetbuf_datalen();
			
			int *p_extra = (int *) malloc (sizeof(int));
			memcpy(p_extra, packetbuf_dataptr(), packetbuf_data_len); //COPIA DEL BUFFER LA CARGA SOBRANTE QUE HA ENVIADO EL NODO
			
			for (nb = list_head(neighbour_table_entry_list); nb != NULL; nb = list_item_next(nb))
			{
				if (linkaddr_cmp(&nb->addr, sender)) //SE BUSCA EN LA LISTA DE VECINOS LA DIRECCIÓN QUE HA ENVIADO EL MENSAJE 
				{	
					//NODO NO EDGE RECIBE CARGA DE SU HIJO O HIJOS
					if (nb->flag == 0 && edge == 0 && n_hijos != 0) 
					{
						//if (*p_extra > 0) //LA CARGA RECIBIDA DEL HIJO ES POSITIVA Y HAY QUE MODIFICAR LA CARGA DEL NODO
						node->load = node->load + *p_extra;	
						
						printf("//INFO INCOMING SHARE// carga actual del nodo: %d\n", node->load);

						n_hijos--; //SE RESTA EL HIJO DE LA CUENTA TOTAL DE HIJOS
						
						if (n_hijos == 0)
						{
							new_edge = 1; //PARA SEGUIR EL ÁRBOL
							ctimer_set(&statistic_timer, IOTORII_STATISTICS2_TIME * CLOCK_SECOND, iotorii_handle_statistic_timer, NULL); //SE MOSTRARÁN LAS ESTADÍSTICAS ACTUALIZADAS		
						
							#if IOTORII_NODE_TYPE == 1
							printf("FIN CONVERGENCIA\n");
							#endif
						}						
					}
					
					if (*p_extra != 0 && nb->in_out == 0) //SI HA HABIDO ACTUALIZACIÓN DE LA CARGA, SE ACTUALIZA EL VALOR DEL "FLUJO DE CARGA"
						nb->in_out = *p_extra;
				}
			}
			
			free(p_extra);
			p_extra = NULL;	
		}
	}
	
	else //ESTÁ ESPECIFICADA
	{
		char *new_hlmac_addr_str = hlmac_addr_to_str(*received_hlmac_addr);
		printf("//INFO INCOMING HLMAC// HLMAC recibida: %s\n", new_hlmac_addr_str);
		free(new_hlmac_addr_str);

		if (!hlmactable_has_loop(*received_hlmac_addr)) //SI NO HAY BUCLE, SI SE LA MANDA EL HIJO
		{
			uint8_t is_added = hlmactable_add(*received_hlmac_addr);

			if (is_added) //SI SE HA ASIGNADO HLMAC AL NODO
			{					
				LOG_DBG("New HLMAC address is assigned to the node.\n");
				LOG_DBG("New HLMAC address is sent to the neighbours.\n");
				iotorii_send_sethlmac(*received_hlmac_addr, sender_link_address); //SE ENVÍA A LOS DEMÁS NODOS
			}
			else //NO SE HA ASIGNADO
			{
				LOG_DBG("New HLMAC address not added to the HLMAC table, and memory is free.\n");
				
				free(received_hlmac_addr->address);
				received_hlmac_addr->address = NULL;
				free(received_hlmac_addr);
				received_hlmac_addr = NULL;
			}
			
			//BÚSQUEDA DE LA DIRECCIÓN DEL EMISOR EN LA LISTA DE VECINOS 
			for (nb = list_head(neighbour_table_entry_list); nb != NULL; nb = list_item_next(nb))
			{
				if (linkaddr_cmp(&nb->addr, sender))
				{
					if (is_added)
						nb->flag = 1; //SE MARCA COMO PADRE ESE VECINO 
					else
						nb->flag = -1; //SE MARCA COMO PADRE ESE VECINO PERO SIN UNIÓN DE PADRE
				}
			}
			number_of_neighbours_flag--; //SE DECREMENTA EL NÚMERO DE VECINOS DE LOS QUE NO SE HA RECIBIDO HLMAC
		}
		else
		{
			LOG_DBG("New HLMAC address not assigned to the node (loop), and memory is free.\n");
			
			free(received_hlmac_addr->address);
			received_hlmac_addr->address = NULL;
			free(received_hlmac_addr);
			received_hlmac_addr = NULL;
		}
	}
	
	#if LOG_DBG_STATISTIC == 1
	//printf("Periodic Statistics: node_id: %u, convergence_time_end\n", node_id);
	#endif
}


void iotorii_operation (void)
{
	if (packetbuf_holds_broadcast())
	{
		//if (hello identification) 
		if (packetbuf_datalen() == 0) //SI LA LONGITUD ES NULA, SE HA RECIBIDO UN PAQUETE HELLO
			iotorii_handle_incoming_hello(); //SE PROCESA PAQUETE HELLO
	
		//else if (SetHLMAC identification or LOAD)
		else //SI NO ES NULA, SE HA RECIBIDO UN PAQUETE SETHLMAC
			iotorii_handle_incoming_sethlmac_or_load(); //SE PROCESA PAQUETE SETHLMAC
	}
}


#endif

/*---------------------------------------------------------------------------*/

#if IOTORII_NODE_TYPE == 1 //ROOT

static void iotorii_handle_sethlmac_timer ()
{
	//uint8_t id = 1;
	hlmacaddr_t root_addr; //SE CREA EL ROOT
	hlmac_create_root_addr(&root_addr, 1);
	hlmactable_add(root_addr);
	
	#if LOG_DBG_STATISTIC == 1
	//printf("Periodic Statistics: node_id: %u, convergence_time_start\n", node_id);
	#endif
	
	iotorii_send_sethlmac(root_addr, linkaddr_node_addr);
	free(root_addr.address); //malloc() in hlmac_create_root_addr()
	root_addr.address = NULL;
}

#endif


static void init (void)
{
	#if LLSEC802154_USES_AUX_HEADER
	
	#ifdef CSMA_LLSEC_DEFAULT_KEY0
	uint8_t key[16] = CSMA_LLSEC_DEFAULT_KEY0;
	csma_security_set_key(0, key);
	#endif
	
	#endif /* LLSEC802154_USES_AUX_HEADER */
	
	csma_output_init(); //INICIALIZA VECINO
	on();
	
	#if IOTORII_NODE_TYPE == 1 //INFORMA QUE ES ROOT
	LOG_INFO("This node operates as a root.\n");
	#endif
	
	#if IOTORII_NODE_TYPE > 1 //INFORMA QUE ES NODO COMÚN
	LOG_INFO("This node operates as a common node.\n ");
	#endif
	
	#if IOTORII_NODE_TYPE > 0 //ROOT O NODO COMÚN

	unsigned short seed_number;
	uint8_t min_len_seed = sizeof(unsigned short) < LINKADDR_SIZE ?  sizeof(unsigned short) : LINKADDR_SIZE;
	
	memcpy(&seed_number, &linkaddr_node_addr, min_len_seed);
	random_init(seed_number); //SE INICIALIZA UN SEED NUMBER DIFERENTE PARA CADA NODO
	
	#if LOG_DBG_DEVELOPER == 1
	LOG_DBG("Seed is %2.2X (%d), sizeof(Seed) is %u\n", seed_number, seed_number, min_len_seed);
	#endif

	//SE PLANIFICA MENSAJE HELLO
	clock_time_t hello_start_time = IOTORII_HELLO_START_TIME * CLOCK_SECOND;
	hello_start_time = hello_start_time / 2 + (random_rand() % (hello_start_time / 2));
	LOG_DBG("Scheduling a Hello message after %u ticks in the future\n", (unsigned)hello_start_time);
	ctimer_set(&hello_timer, hello_start_time, iotorii_handle_hello_timer, NULL);

	number_of_neighbours = 0;
	number_of_neighbours_flag = 0;
	hlmac_table_init(); //SE CREA LA TABLA DE VECINOS
	
	//ESTADÍSTICAS
	#if LOG_DBG_STATISTIC == 1
	ctimer_set(&statistic_timer, IOTORII_STATISTICS1_TIME * CLOCK_SECOND, iotorii_handle_statistic_timer, NULL);
	#endif
	
	#endif

	#if IOTORII_NODE_TYPE == 1 //ROOT
	//SE PLANIFICA MENSAJE SETHLMAC EN CASO DE SER ROOT
	ctimer_set(&sethlmac_timer, IOTORII_SETHLMAC_START_TIME * CLOCK_SECOND, iotorii_handle_sethlmac_timer, NULL);
	#endif
}


static void input_packet (void) //SE RECIBE UN PAQUETE 
{
	#if CSMA_SEND_SOFT_ACK
	uint8_t ackdata[CSMA_ACK_LEN];
	#endif

	if (packetbuf_datalen() == CSMA_ACK_LEN) //SE IGNORAN PAQUETES ACK
	{
		LOG_DBG("ignored ack\n");
	} 
	else if (csma_security_parse_frame() < 0) //ERROR AL PARSEAR
	{
		LOG_ERR("failed to parse %u\n", packetbuf_datalen());
	} 
	else if (!linkaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER), &linkaddr_node_addr) && !packetbuf_holds_broadcast()) //NO COINCIDE LA DIRECCIÓN
	{
		LOG_WARN("not for us\n");
	} 
	else if (linkaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_SENDER), &linkaddr_node_addr)) //PAQUETE DEL PROPIO NODO
	{
		LOG_WARN("frame from ourselves\n");
	} 
	else 
	{
		int duplicate = 0;

		duplicate = mac_sequence_is_duplicate(); //SE COMPRUEBA SI ESTÁ DUPLICADO
		
		if (duplicate) //SI LO ESTÁ, SE BORRA
		{
			LOG_WARN("drop duplicate link layer packet from ");
			LOG_WARN_LLADDR(packetbuf_addr(PACKETBUF_ADDR_SENDER));
			LOG_WARN_(", seqno %u\n", packetbuf_attr(PACKETBUF_ATTR_MAC_SEQNO));
		} 
		else 
		{
		    mac_sequence_register_seqno(); //SE REGISTRA
		}

		#if CSMA_SEND_SOFT_ACK
		if (packetbuf_attr(PACKETBUF_ATTR_MAC_ACK)) 
		{
			ackdata[0] = FRAME802154_ACKFRAME;
			ackdata[1] = 0;
			ackdata[2] = ((uint8_t*) packetbuf_hdrptr())[2];
			NETSTACK_RADIO.send(ackdata, CSMA_ACK_LEN); //SE ENVÍA ACK
		}
		#endif /* CSMA_SEND_SOFT_ACK */
		
		if (!duplicate)  //SI NO ESTÁ DUPLICADO
		{
			LOG_INFO("received packet from ");
			LOG_INFO_LLADDR(packetbuf_addr(PACKETBUF_ADDR_SENDER));
			LOG_INFO_(", seqno %u, len %u\n", packetbuf_attr(PACKETBUF_ATTR_MAC_SEQNO), packetbuf_datalen());

			#if IOTORII_NODE_TYPE == 0
			NETSTACK_NETWORK.input();
			
			#elif IOTORII_NODE_TYPE > 0
			iotorii_operation();
			
			#endif
		}
	}
}


#if IOTORII_NODE_TYPE == 0
const struct mac_driver csma_driver = { "CSMA",

#elif IOTORII_NODE_TYPE > 0
const struct mac_driver iotorii_csma_driver = { "IoTorii CSMA",
#endif

	init,
	send_packet,
	input_packet,
	on,
	off,
	max_payload,
};

char *link_addr_to_str (const linkaddr_t addr, int length)
{
	char *address = (char*) malloc (sizeof(char) * (length * (2 + 1) + 1)); //SE AÑADE ESPACIO PARA LOS . Y PARA '\0'
	char *s = (char *)address;
	uint8_t i;
	
	for (i = 0; i < length; i++, s += 3)
		sprintf(s, "%2.2X.", (char)addr.u16[i]);

	*(s) = '\0'; //SE AÑADE '\0' AL FINAL
	
	return address;
}


void list_this_node_entry (payload_entry_t *a, hlmacaddr_t *addr)
{
	payload_entry_t *aux = a;
	this_node_t *new_address;
	
	char* str_addr = hlmac_addr_to_str(*addr);
	char* str_top_addr = (char*) malloc (20);

	if (addr->len > 1)
		strncpy(str_top_addr, str_addr, (addr->len-1)*3); //COPIA HASTA EL PENÚLTIMO ID
	else
		str_top_addr = "/";
	
	while (aux != NULL)
	{		
		new_address = (this_node_t*) malloc (sizeof(this_node_t)); //SE RESERVA ESPACIO
		
		new_address->str_addr = str_addr;		
		new_address->str_top_addr = str_top_addr;	
		
		new_address->payload = *aux->payload;
		new_address->nhops = hlmactable_calculate_sum_hop();
		new_address->load = random_rand() % 200; //CARGA ALEATORIA (SE HA IMPUESTO UN MAX DE 180)
		
		list_add(node_list, new_address); 
		printf("[addr: %s | top_addr: %s | payload/hops: %d | node_load: %d]\n", new_address->str_addr, new_address->str_top_addr, new_address->nhops, new_address->load);
		aux = aux->next;
	}
}