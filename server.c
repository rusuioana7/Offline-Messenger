#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sqlite3.h>

#define SFARSIT_TRANSMISIUNE "Transmisiune Incheiata\0"
#define PORT 2908
#define BAZA_DE_DATE "db.sqlite"
#define COMANDA_LOGIN "SELECT * FROM utilizatori WHERE nume='%s' AND parola='%s';"
#define COMANDA_CREATE "CREATE TABLE IF NOT EXISTS %s (id INTEGER PRIMARY KEY, nume_utilizator TEXT, mesaj TEXT);"
#define COMANDA_INSERT "INSERT INTO %s (nume_utilizator, mesaj) VALUES ('%s', '%s');"
#define COMANDA_SELECT_MESAJE "SELECT * FROM %s;"
#define COMANDA_SELECT_CONTACTE "SELECT nume FROM utilizatori;"
#define COMANDA_RASPUNDE "UPDATE %s SET mesaj = mesaj || '(' || (SELECT mesaj from %s where id = %d) || ')' WHERE id = (SELECT MAX(id) FROM %s);"
#define COMANDA_STERGE "DELETE from %s;"
#define COMANDA_EXISTA_NOTIFICARI "SELECT * FROM notificari WHERE pentru='%s';"
#define COMANDA_CAUTA_NOTIFICARI "SELECT * FROM notificari WHERE pentru='%s';"
#define COMANDA_INSERT_NOTIFICARI "INSERT OR IGNORE INTO notificari (dela, pentru) VALUES ('%s', '%s');"
#define COMANDA_DELETE_NOTIFICARI "DELETE FROM notificari WHERE pentru='%s';"

extern int errno; // codul de eroare returnat
typedef struct thData {
    int idThread; // id-ul thread-ului tinut in evidenta de acest program
    int cl;       // descriptorul intors de accept
} thData;

static sqlite3 *db; // pointer catre baza de date

static void initializare_baza_de_date();

static void create_table(const char *);

static void insert_mesaj(const char *, const char *, const char *);

static void afiseaza_contacte(int);

static void afiseaza_mesaje(const char *, const char *, int);

static void scrie_mesaj(const char *, const char *, const char *);

static void raspunde_la_mesaj(const char *, const char *, const char *, int);

static void sterge_mesaje(const char *, const char *);

static int login_db(char *, char *);

void dupa_trimitere_mesaj(const char *, const char *);

void trimite_notificari_mesaje_primite(const char *, int);

static void *treat(void *);

// cod preluat(partea de realizare a conexiunii) si modificat din: https://profs.info.uaic.ro/~computernetworks/files/NetEx/S12/ServerConcThread/servTcpConcTh2.c
int main() {
    initializare_baza_de_date();

    struct sockaddr_in server; // structura folosita de server
    struct sockaddr_in from;
    int nr; // mesajul primit de trimis la client
    int sd; // descriptorul de socket
    int pid;
    pthread_t th[100]; // Identificatorii thread-urilor care se vor crea
    int i = 0;

    /* crearea unui socket */
    if ((sd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("[server]Eroare la socket().\n");
        return -1;
    }
    /* utilizarea optiunii SO_REUSEADDR */
    int on = 1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    /* pregatirea structurilor de date */
    bzero(&server, sizeof(server));
    bzero(&from, sizeof(from));
    /* umplem structura folosita de server */
    /* stabilirea familiei de socket-uri */
    server.sin_family = AF_INET;
    /* acceptam orice adresa */
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    /* utilizam un port utilizator */
    server.sin_port = htons(PORT);
    /* atasam socketul */
    if (bind(sd, (struct sockaddr *) &server, sizeof(struct sockaddr)) == -1) {
        perror("[server]Eroare la bind().\n");
        return -1;
    }
    /* serverul asculta daca vin clienti sa se conecteze */
    if (listen(sd, 2) == -1) {
        perror("[server]Eroare la listen().\n");
        return -1;
    }

    printf("[server]Asteptam la portul %d...\n", PORT);
    fflush(stdout);

    /* servim in mod concurent clientii folosind thread-uri */
    while (1) {
        int client;
        thData *td;
        int length = sizeof(from);

        /* acceptam un client (stare blocanta pina la realizarea conexiunii) */
        if ((client = accept(sd, (struct sockaddr *) &from, &length)) < 0) {
            perror("[server]Eroare la accept().\n");
            continue;
        }

        /* s-a realizat conexiunea, se astepta mesajul */
        // int idThread; //id-ul threadului
        // int cl; //descriptorul intors de accept

        td = (struct thData *) malloc(sizeof(struct thData));
        td->idThread = i++;
        td->cl = client;

        pthread_create(&th[i], NULL, &treat, td);

    }//while

    sqlite3_close(db);

    return 0;
}

static void *treat(void *arg) {
    struct thData tdL;
    tdL = *((struct thData *) arg);

    printf("[thread]- %d - Asteptam mesajul...\n", tdL.idThread);
    fflush(stdout);
    pthread_detach(pthread_self());

    //declarare variabile
    char buf_citire[100];
    char buf_scriere[100];
    int utilizator_autentificat = 0;
    char nume_utilizator[94];
    int fd = tdL.cl;
    memset(nume_utilizator, '\0', sizeof(nume_utilizator));

    while (1) {
        // se citeste comanda de la client
        int read_chars = 0;
        memset(buf_citire, '\0', sizeof(buf_citire));
        read_chars = read(tdL.cl, &buf_citire, sizeof(buf_citire));

        // se verifica daca citirea s-a facut cu succes
        if (read_chars <= 0) {
            printf("[Thread %d] Error - disconnected\n", tdL.idThread);
            break;
        }

        // se afiseaza mesajul receptionat
        printf("[Thread %d]Mesajul a fost receptionat: %s", tdL.idThread, buf_citire);

        // se cauta comanda corecta ce trebuie executata:
        switch (utilizator_autentificat) {
            // user ul nu e conectat
            case 0:
                if (!strncmp(buf_citire, "login ", 6)) { //login
                    char login[6], parola[100];

                    if (sscanf(buf_citire, "%s %s %s", login, nume_utilizator, parola) != 3) {
                        sprintf(buf_scriere, "Comanda incorecta. Pentru ajutor, scrie help\n");
                        write(fd, buf_scriere, sizeof(buf_scriere));
                        break;
                    }

                    // se face autentificarea
                    utilizator_autentificat = login_db(nume_utilizator, parola);
                    if (utilizator_autentificat) {
                        // user ul este autentificat
                        sprintf(buf_scriere, "Buna %s\n", nume_utilizator);
                        write(fd, buf_scriere, sizeof(buf_scriere));

                        trimite_notificari_mesaje_primite(nume_utilizator, fd);
                    } else {
                        // user ul nu e autentificat
                        sprintf(buf_scriere, "Numele sau parola sunt incorecte.\n");
                        write(fd, buf_scriere, sizeof(buf_scriere));
                    }
                } else if (!strncmp(buf_citire, "help", 4)) { //help
                    sprintf(buf_scriere,
                            "Te rog sa te autentifici pentru a avea acces la comenzi\nHint: login <nume_utilizator> <parola>\n\n");
                    write(fd, buf_scriere, sizeof(buf_scriere));
                } else {
                    // caz default, cand nu e recunsocuta nicio comanda
                    sprintf(buf_scriere, "Comanda incorecta. Pentru ajutor, scrie help\n");
                    write(fd, buf_scriere, sizeof(buf_scriere));
                }
                break;

            case 1: // user ul este conectat
                if (!strncmp(buf_citire, "logout", 6)) { // logout
                    sprintf(buf_scriere, "Bye %s\n", nume_utilizator);
                    write(fd, buf_scriere, sizeof(buf_scriere));

                    // se reseteaza valorile pentru utilizator
                    utilizator_autentificat = 0;
                    memset(nume_utilizator, '\0', sizeof(nume_utilizator));
                } else if (!strncmp(buf_citire, "help", 4)) { //help
                    sprintf(buf_scriere, "Esti autentificat ca: ");
                    write(fd, buf_scriere, sizeof(buf_scriere));

                    sprintf(buf_scriere, "%s\n", nume_utilizator);
                    write(fd, buf_scriere, sizeof(buf_scriere));

                    sprintf(buf_scriere, "Comenzi disponibile\nlogout : delogheaza de pe server\n");
                    write(fd, buf_scriere, sizeof(buf_scriere));

                    sprintf(buf_scriere, "contacte : afiseaza numele contactelor\n");
                    write(fd, buf_scriere, sizeof(buf_scriere));

                    sprintf(buf_scriere,
                            "mesaje <nume_contact> : afiseaza mesajele din istoric cu contactul <nume_contact>\n");
                    write(fd, buf_scriere, sizeof(buf_scriere));

                    sprintf(buf_scriere, "scrie <nume_contact> <mesaj> : scrie un <mesaj> către <nume_contact>\n");
                    write(fd, buf_scriere, sizeof(buf_scriere));

                    sprintf(buf_scriere,
                            "raspunde <nume_contact> <id_mesaj> <mesaj> : raspunde la mesajul <id_mesaj>\n");
                    write(fd, buf_scriere, sizeof(buf_scriere));

                    sprintf(buf_scriere, "sterge <nume_contact> : sterge istoricul pentru <nume_contact>\n\n");
                    write(fd, buf_scriere, sizeof(buf_scriere));
                } else if (!strncmp(buf_citire, "contacte", 8)) { // utilizatorii disponibili
                    afiseaza_contacte(fd);
                } else if (!strncmp(buf_citire, "mesaje ", 7)) { // istoricul conv cu alt user
                    char mesaje[7], nume_contact[100];
                    if (sscanf(buf_citire, "%s %s", mesaje, nume_contact) != 2) {
                        sprintf(buf_scriere, "Comanda incorecta. Pentru ajutor, scrie help\n");
                        write(fd, buf_scriere, sizeof(buf_scriere));
                        break;
                    }

                    afiseaza_mesaje(nume_utilizator, nume_contact, fd);
                } else if (!strncmp(buf_citire, "scrie ", 6)) { // scrie un mesaj catre un alt user
                    char scrie[6], nume_contact[100], mesaj[80];
                    if (sscanf(buf_citire, "%s %s %s", scrie, nume_contact, mesaj) != 3) {
                        sprintf(buf_scriere, "Comanda incorecta. Pentru ajutor, scrie help\n");
                        write(fd, buf_scriere, sizeof(buf_scriere));
                        break;
                    }

                    strcpy(mesaj, strstr(buf_citire, mesaj));
                    mesaj[strlen(mesaj) - 1] = '\0';

                    scrie_mesaj(nume_utilizator, nume_contact, mesaj);

                    sprintf(buf_scriere, "trimis:%s\n", mesaj);
                    write(fd, buf_scriere, sizeof(buf_scriere));
                } else if (!strncmp(buf_citire, "raspunde ", 9)) { //reply la un anumit mesaj
                    int id_mesaj;
                    char raspunde[9], nume_contact[100], mesaj[80];
                    if (sscanf(buf_citire, "%s %s %d %s", raspunde, nume_contact, &id_mesaj, mesaj) != 4) {
                        sprintf(buf_scriere, "Comanda incorecta. Pentru ajutor, scrie help\n");
                        write(fd, buf_scriere, sizeof(buf_scriere));
                        break;
                    }

                    strcpy(mesaj, strstr(buf_citire, mesaj));
                    mesaj[strlen(mesaj) - 1] = '\0';

                    raspunde_la_mesaj(nume_utilizator, nume_contact, mesaj, id_mesaj);

                    sprintf(buf_scriere, "trimis:%s\n", mesaj);
                    write(fd, buf_scriere, sizeof(buf_scriere));
                } else if (!strncmp(buf_citire, "sterge ", 7)) { //sterge istoricul conv cu un user
                    int id_mesaj;
                    char sterge[7], nume_contact[100];
                    if (sscanf(buf_citire, "%s %s", sterge, nume_contact) != 2) {
                        sprintf(buf_scriere, "Comanda incorecta. Pentru ajutor, scrie help\n");
                        write(fd, buf_scriere, sizeof(buf_scriere));
                        break;
                    }

                    sterge_mesaje(nume_utilizator, nume_contact);

                    sprintf(buf_scriere, "sters\n");
                    write(fd, buf_scriere, sizeof(buf_scriere));
                } else {
                    // default, pentru cazul in care mesajul de la client nu se potriveste la nicio comanda
                    sprintf(buf_scriere, "Comanda incorecta. Pentru ajutor, scrie help\n");
                    write(fd, buf_scriere, sizeof(buf_scriere));
                }
                break;
        }

        write(fd, SFARSIT_TRANSMISIUNE, 24);
    }


    return (NULL);
};

// conectare la baza de date
static void initializare_baza_de_date() {
    int rc = sqlite3_open(BAZA_DE_DATE, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
    }
}

// callback general, care trimite tot continutul query-ului la client.
static int callback_general(void *data, int argc, char **argv, char **azColName) {
    int fd = *((int *) data);
    char buf[100];

    for (int i = 0; i < argc; i++) {
        sprintf(buf, "%s ", argv[i]);
        write(fd, buf, sizeof(buf));
    }

    sprintf(buf, "\n");
    write(fd, buf, sizeof(buf));

    return 0;
}

// callback pentru trimite mesaje.
static int callback_trimite_mesaje(void *data, int argc, char **argv, char **azColName) {
    //declarare variabile
    int fd = *((int *) data);
    char buf[100];

    // trimitere mesaje formatate catre client (id, nume_utilizator, mesaj)
    sprintf(buf, "%s\t%s:%s\n", argv[0], argv[1], argv[2]);
    write(fd, buf, sizeof(buf));

    return 0;
}

// callback pentru login.
int login_callback(void *data, int argc, char **argv, char **azColName) {
    int *found = (int *) data;

    if (argc == 2) {
        *found = 1;
    }

    return 0;
}

// functie pentru crearea unei tabele daca nu exista
static void create_table(const char *nume_canal) {
    char comanda_sql[256];
    sprintf(comanda_sql, COMANDA_CREATE, nume_canal);

    int rc = sqlite3_exec(db, comanda_sql, callback_general, 0, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot create table: %s\n", sqlite3_errmsg(db));
    }
}



static void insert_mesaj(const char *nume_canal, const char *nume_utilizator, const char *mesaj) {
    char comanda_sql[256];
    sprintf(comanda_sql, COMANDA_INSERT, nume_canal, nume_utilizator, mesaj);

    int rc = sqlite3_exec(db, comanda_sql, callback_general, 0, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot insert data: %s\n", sqlite3_errmsg(db));
    }
}

// verifica daca utilizatorul exista in baza de date si e identificat de parola
static int login_db(char *nume_utilizator, char *parola) {
    int rc;
    int found = 0;
    char comanda_sql[100];

    snprintf(comanda_sql, sizeof(comanda_sql), COMANDA_LOGIN, nume_utilizator, parola);
    rc = sqlite3_exec(db, comanda_sql, login_callback, &found, NULL);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(db));
    }

    return found;
}


// interogheaza tabela UTILIZATORI pentru comanda 'contacte'
static void afiseaza_contacte(int fd) {
    char comanda_sql[256];

    sprintf(comanda_sql, COMANDA_SELECT_CONTACTE);
    int rc = sqlite3_exec(db, comanda_sql, callback_general, &fd, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot select data: %s\n", sqlite3_errmsg(db));
    }
}

// se calculeaza numele tabelei bazat pe numele utilizatorului logat si numele contactului
static void denumire_canal(const char *nume_utilizator, const char *nume_contact, char *nume_canal) {
    if (strcmp(nume_utilizator, nume_contact) > 0) {
        strcpy(nume_canal, nume_utilizator);
        strcat(nume_canal, nume_contact);
    } else {
        strcpy(nume_canal, nume_contact);
        strcat(nume_canal, nume_utilizator);
    }
}

// pentru comanda 'mesaje'
static void afiseaza_mesaje(const char *nume_utilizator, const char *nume_contact, int fd) {
    char nume_canal[100];
    char comanda_sql[256];
    denumire_canal(nume_utilizator, nume_contact, nume_canal);
    sprintf(comanda_sql, COMANDA_SELECT_MESAJE, nume_canal);
    int rc = sqlite3_exec(db, comanda_sql, callback_trimite_mesaje, &fd, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot select data: %s\n", sqlite3_errmsg(db));
    }
}

// trimite mesaj de la utilizatorul autentificat la contact, pentru comanda 'scrie'
static void scrie_mesaj(const char *nume_utilizator, const char *nume_contact, const char *mesaj) {
    char nume_canal[100];
    denumire_canal(nume_utilizator, nume_contact, nume_canal);// se calculeaza numele tabelei
    create_table(nume_canal);
    insert_mesaj(nume_canal, nume_utilizator, mesaj);
    dupa_trimitere_mesaj(nume_utilizator, nume_contact);
}



void adaugare_mesaj_raspuns(const char *nume_canal, int id_mesaj) {
    char comanda_sql[256];

    sprintf(comanda_sql, COMANDA_RASPUNDE, nume_canal, nume_canal, id_mesaj, nume_canal);
    int rc = sqlite3_exec(db, comanda_sql, callback_general, 0, NULL);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot insert data: %s\n", sqlite3_errmsg(db));
    }
}

// comanda 'reply'. Se permite raspunderea la un mesaj identificat prin id_mesaj.
static void raspunde_la_mesaj(const char *nume_utilizator, const char *nume_contact, const char *mesaj, int id_mesaj) {
    char nume_canal[100];

    denumire_canal(nume_utilizator, nume_contact, nume_canal);
    create_table(nume_canal);
    insert_mesaj(nume_canal, nume_utilizator, mesaj);
    adaugare_mesaj_raspuns(nume_canal, id_mesaj);
}

// Sterge mesajele dintre utilizator si contact, comanda 'sterge'
static void sterge_mesaje(const char *nume_utilizator, const char *nume_contact) {
    char comanda_sql[256];
    char nume_canal[100];

    denumire_canal(nume_utilizator, nume_contact, nume_canal);
    sprintf(comanda_sql, COMANDA_STERGE, nume_canal);

    int rc = sqlite3_exec(db, comanda_sql, callback_general, 0, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot delete data: %s\n", sqlite3_errmsg(db));
    }
}


int notificare_callback(void *data, int argc, char **argv, char **azColName) {
    int *found = (int *) data;

    if (argc == 2) {
        *found = 1;
    }

    return 0;
}

static int callback_trimite_notificari(void *data, int argc, char **argv, char **azColName) {
    int fd = *((int *) data);
    char buf[100];

    sprintf(buf, "%s, ", argv[0]);
    write(fd, buf, sizeof(buf));

    return 0;
}

void dupa_trimitere_mesaj(const char *nume_utilizator, const char *nume_contact) {

    char comanda_sql[256];
    sprintf(comanda_sql, COMANDA_INSERT_NOTIFICARI, nume_utilizator, nume_contact);

    int rc = sqlite3_exec(db, comanda_sql, callback_general, 0, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot insert data: %s\n", sqlite3_errmsg(db));
    }
}

void trimite_notificari(const char *nume_utilizator, int fd) {
    char comanda_sql[256];

    sprintf(comanda_sql, COMANDA_CAUTA_NOTIFICARI, nume_utilizator);
    int rc = sqlite3_exec(db, comanda_sql, callback_trimite_notificari, &fd, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot select data: %s\n", sqlite3_errmsg(db));
    }
}

int are_notificari(const char *nume_utilizator) {
    int rc;
    int found = 0;
    char comanda_sql[100];

    snprintf(comanda_sql, sizeof(comanda_sql), COMANDA_EXISTA_NOTIFICARI, nume_utilizator);
    rc = sqlite3_exec(db, comanda_sql, notificare_callback, &found, NULL);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(db));
    }

    return found;
}

void stergere_notificari(const char *nume_utilizator) {
    char comanda_sql[256];
    sprintf(comanda_sql, COMANDA_DELETE_NOTIFICARI, nume_utilizator);

    int rc = sqlite3_exec(db, comanda_sql, callback_general, 0, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot delete data: %s\n", sqlite3_errmsg(db));
    }
}

// Verifica daca utilizatorul are mesaje si apoi trimite numele utilizatorilor
void trimite_notificari_mesaje_primite(const char *nume_utilizator, int fd) {
    char buf_trimitere[100];
    if (are_notificari(nume_utilizator)) {
        sprintf(buf_trimitere, "Aveti notificari de la: ");
        write(fd, buf_trimitere, sizeof(buf_trimitere));

        trimite_notificari(nume_utilizator, fd);

        stergere_notificari(nume_utilizator);
    } else {
        sprintf(buf_trimitere, "Nu aveti notificari...\n");
        write(fd, buf_trimitere, sizeof(buf_trimitere));
    }
}



