// Sonde tvOS : verifie que l'Apple TV accepte la memoire du jeu (4 Go reserves, 3 vues de la meme memoire
// physique), mesure la memoire disponible et les dossiers ou l'on peut ecrire.
#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <unistd.h>

static void Ecrire(const char* nom, NSSearchPathDirectory dossier)
{
    NSString* chemin = NSSearchPathForDirectoriesInDomains(dossier, NSUserDomainMask, YES).firstObject;
    if (chemin == nil) { printf("[dossier] %s : introuvable\n", nom); return; }
    [[NSFileManager defaultManager] createDirectoryAtPath:chemin withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* fichier = [chemin stringByAppendingPathComponent:@"essai.bin"];
    FILE* f = fopen(fichier.UTF8String, "wb");
    printf("[dossier] %s (%s) : %s\n", nom, chemin.UTF8String, f ? "ecriture OK" : strerror(errno));
    if (f) { fclose(f); unlink(fichier.UTF8String); }
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        uint64_t memsize = 0; size_t len = sizeof(memsize);
        sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
        int p0 = 0, p1 = 0; len = sizeof(int);
        sysctlbyname("hw.perflevel0.physicalcpu", &p0, &len, NULL, 0); len = sizeof(int);
        sysctlbyname("hw.perflevel1.physicalcpu", &p1, &len, NULL, 0);
        printf("[machine] memoire %llu Mo, coeurs performance %d, efficacite %d\n", memsize >> 20, p0, p1);
        printf("[memoire] disponible pour l'app : %zu Mo\n", os_proc_available_memory() >> 20);

        for (uint64_t go = 4; go <= 64; go *= 2)
        {
            void* p = mmap(NULL, go << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
            printf("[espace] reservation %llu Go : %s\n", go, p == MAP_FAILED ? strerror(errno) : "OK");
            if (p != MAP_FAILED) munmap(p, go << 30);
        }

        uint8_t* base = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (base == MAP_FAILED) { printf("[espace] 4 Go impossible, arret\n"); return 1; }

        int fd = shm_open("/simpsons-sonde", O_RDWR | O_CREAT | O_EXCL, 0600);
        printf("[vues] shm_open : %s\n", fd < 0 ? strerror(errno) : "OK");
        if (fd >= 0) { shm_unlink("/simpsons-sonde"); close(fd); }

        // Vues par vm_remap : la memoire de 0xA0000000 est reprojetee a 0xC0000000 et 0xE0000000
        const uint64_t taille = 0x20000000;
        bool vues = true;
        for (uint64_t adresse = 0xC0000000; adresse <= 0xE0000000; adresse += 0x20000000)
        {
            vm_address_t cible = (vm_address_t)(base + adresse);
            vm_prot_t cur, max;
            kern_return_t kr = vm_remap(mach_task_self(), &cible, taille, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                mach_task_self(), (vm_address_t)(base + 0xA0000000), FALSE, &cur, &max, VM_INHERIT_NONE);
            printf("[vues] vm_remap 0x%llX : %s\n", adresse, kr == KERN_SUCCESS ? "OK" : mach_error_string(kr));
            vues = vues && kr == KERN_SUCCESS && cible == (vm_address_t)(base + adresse);
        }
        if (vues)
        {
            base[0xA0001234] = 0x5A; base[0xBFFFFFF0] = 0x77;
            bool alias = base[0xC0001234] == 0x5A && base[0xE0001234] == 0x5A && base[0xFFFFFFF0] == 0x77;
            base[0xE0000010] = 0x33;
            alias = alias && base[0xA0000010] == 0x33;
            printf("[vues] meme memoire vue aux trois adresses : %s\n", alias ? "OUI" : "NON");
        }

        // Remplissage progressif de la RAM du jeu (bas de l'espace) pour trouver la limite reelle
        size_t touche = 0;
        for (size_t mo = 0; mo < 3072; mo += 64)
        {
            if (os_proc_available_memory() < (200u << 20)) break;
            memset(base + 0x10000000 + (mo << 20), 1, 64u << 20);
            touche = mo + 64;
        }
        printf("[memoire] %zu Mo remplis, il reste %zu Mo\n", touche, os_proc_available_memory() >> 20);
        memset(base + 0x10000000, 0, touche << 20);
        madvise(base + 0x10000000, touche << 20, MADV_FREE);

        Ecrire("Documents", NSDocumentDirectory);
        Ecrire("Application Support", NSApplicationSupportDirectory);
        Ecrire("Caches", NSCachesDirectory);
        printf("[sonde] fin\n");
        fflush(stdout);
    }
    return 0;
}
