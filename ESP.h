


static uintptr_t GetUE4Base() {
    uintptr_t ue4_base = 0;
    FILE* maps = fopen("/proc/self/maps", "r");
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
        if (strstr(line, "libUE4.so") && strstr(line, "r-xp")) {
            sscanf(line, "%" SCNx64, &ue4_base);
            break;
        }
    }
    fclose(maps);
    return ue4_base;
}

static uintptr_t findSymbol(const char* symName) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;

    uintptr_t ue4_base = 0;
    char line[512], mapPath[512] = {0};

    while (fgets(line, sizeof(line), maps)) {
        if (strstr(line, "libUE4.so") && strstr(line, "r-xp")) {
            char* pathStart = strchr(line, '/');
            if (pathStart) {
                char* nl = strchr(pathStart, '\n');
                if (nl) *nl = '\0';
                strncpy(mapPath, pathStart, sizeof(mapPath) - 1);
            }
            sscanf(line, "%" SCNx64, &ue4_base);
            break;
        }
    }
    fclose(maps);
    if (!ue4_base || mapPath[0] == '\0') return 0;

    FILE* elf = fopen(mapPath, "rb");
    if (!elf) return 0;

    Elf64_Ehdr ehdr;
    if (fread(&ehdr, sizeof(ehdr), 1, elf) != 1) { fclose(elf); return 0; }
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) { fclose(elf); return 0; }

    Elf64_Shdr* sections = (Elf64_Shdr*)malloc(ehdr.e_shentsize * ehdr.e_shnum);
    fseek(elf, ehdr.e_shoff, SEEK_SET);
    fread(sections, ehdr.e_shentsize, ehdr.e_shnum, elf);

    Elf64_Shdr* shstrSec = &sections[ehdr.e_shstrndx];
    char* shstrtab = (char*)malloc(shstrSec->sh_size);
    fseek(elf, shstrSec->sh_offset, SEEK_SET);
    fread(shstrtab, shstrSec->sh_size, 1, elf);

    Elf64_Shdr* dynsym = NULL, *dynstr = NULL;
    for (int i = 0; i < ehdr.e_shnum; i++) {
        char* name = shstrtab + sections[i].sh_name;
        if (strcmp(name, ".dynsym") == 0) dynsym = &sections[i];
        if (strcmp(name, ".dynstr") == 0) dynstr = &sections[i];
    }

    uintptr_t result = 0;

    if (dynsym && dynstr) {
        char* strtab = (char*)malloc(dynstr->sh_size);
        fseek(elf, dynstr->sh_offset, SEEK_SET);
        fread(strtab, dynstr->sh_size, 1, elf);

        Elf64_Sym* syms = (Elf64_Sym*)malloc(dynsym->sh_size);
        fseek(elf, dynsym->sh_offset, SEEK_SET);
        fread(syms, dynsym->sh_size, 1, elf);

        int numSyms = dynsym->sh_size / sizeof(Elf64_Sym);

        for (int i = 0; i < numSyms; i++) {
            if (syms[i].st_name >= dynstr->sh_size) continue;
            char* name = strtab + syms[i].st_name;

            if (strcmp(name, symName) == 0) {
                result = ue4_base + syms[i].st_value;
                break;
            }
        }
        free(strtab);
        free(syms);
    }

    free(shstrtab);
    free(sections);
    fclose(elf);
    return result;
}

struct ESPData {
    float ScreenX, ScreenY;
    float Distance;
    bool bValid;
    bool bIsPlayer;
    bool bIsTamed;
    char Name[64];
    float Health;
    float MaxHealth;
    float ScreenX2, ScreenY2;
};

static bool g_ESP_Enabled = false;
static bool g_ESP_Players = true;
static bool g_ESP_WildDinos = true;
static bool g_ESP_TamedDinos = true;
static bool g_ESP_Boxes = true;
static bool g_ESP_Snaplines = true;
static bool g_ESP_Distance = true;
static bool g_ESP_Names = true;
static bool g_ESP_Health = true;
static float g_ESP_MaxDistance = 300.0f;

static ESPData g_ESPData[1000];
static int g_ESPCount = 0;
static pthread_mutex_t g_ESPMutex = PTHREAD_MUTEX_INITIALIZER;



static uintptr_t GetGWorld() {
    static uintptr_t GWorldSym = 0;
    static uintptr_t lastGWorld = 0;

    if (!GWorldSym) {
        GWorldSym = findSymbol("GWorld");
        if (!GWorldSym) return 0;
    }

    uintptr_t GWorld = *(uintptr_t*)GWorldSym & 0x00FFFFFFFFFFFFFF;


    if (GWorld != lastGWorld || GWorld == 0) {
        lastGWorld = GWorld;
        return GWorld;
    }

    return GWorld;
}



static int g_ESP_PlayerCount = 0;
static int g_ESP_TamedCount = 0;
static int g_ESP_WildCount = 0;
static int g_ESP_TotalCount = 0;


static void* ESPThread(void*) {
    LOGI("ESP Thread: waiting 10s...");
    sleep(10);

    uintptr_t ue4_base = GetUE4Base();

    uintptr_t cachedGWorld = 0;
    int gworldTimer = 0;

    while (true) {
        if (!g_ESP_Enabled) { usleep(100000); continue; }

        if (gworldTimer <= 0 || cachedGWorld == 0) {
            uintptr_t newGWorld = GetGWorld();
            if (!newGWorld) { usleep(100000); continue; }
            cachedGWorld = newGWorld;
            gworldTimer = 30;
        }
        gworldTimer--;

        uintptr_t GWorld = cachedGWorld;
        if (!GWorld || *(uintptr_t*)GWorld == 0) { cachedGWorld = 0; usleep(100000); continue; }

        uintptr_t PersistentLevel = *(uintptr_t*)(GWorld + 0x58) & 0x00FFFFFFFFFFFFFF;
        if (!PersistentLevel) { cachedGWorld = 0; usleep(100000); continue; }

        uintptr_t Actors = *(uintptr_t*)(PersistentLevel + 0xA0) & 0x00FFFFFFFFFFFFFF;
        int Count = *(int*)(PersistentLevel + 0xA8);
        if (!Actors || Count <= 0 || Count > 50000) { cachedGWorld = 0; usleep(100000); continue; }

        uintptr_t GI = *(uintptr_t*)(GWorld + 0x1A8) & 0x00FFFFFFFFFFFFFF;
        if (!GI) { usleep(50000); continue; }
        uintptr_t LP_Array = *(uintptr_t*)(GI + 0x38) & 0x00FFFFFFFFFFFFFF;
        if (!LP_Array) { usleep(50000); continue; }
        uintptr_t LP = *(uintptr_t*)(LP_Array) & 0x00FFFFFFFFFFFFFF;
        if (!LP) { usleep(50000); continue; }
        uintptr_t PC = *(uintptr_t*)(LP + 0x30) & 0x00FFFFFFFFFFFFFF;
        if (!PC) { usleep(50000); continue; }

        uintptr_t myPawn = *(uintptr_t*)(PC + 0x5D8) & 0x00FFFFFFFFFFFFFF;

        uintptr_t CamMgr = *(uintptr_t*)(PC + 0x660) & 0x00FFFFFFFFFFFFFF;
        if (!CamMgr) { usleep(50000); continue; }

        float* cam = (float*)(CamMgr + 0xBD0);
        float camX = cam[4], camY = cam[5], camZ = cam[6];
        float camPitch = cam[7], camYaw = cam[8];
        float camFOV = cam[14];
        if (camFOV < 10 || camFOV > 500) camFOV = 90.0f;

        float fovRad = camFOV * M_PI / 180.0f;
        float tanHalfFOV = tanf(fovRad / 2.0f);
        if (tanHalfFOV < 0.001f) tanHalfFOV = 0.001f;

        float aspectRatio = (float)screenWidth / (float)screenHeight;
        float scaleX = (float)screenWidth / (2.0f * tanHalfFOV);
        float scaleY = (float)screenHeight / (2.0f * tanHalfFOV);
        if (aspectRatio > 1.0f) scaleY *= aspectRatio;

        float pitchRad = camPitch * M_PI / 180.0f;
        float yawRad = camYaw * M_PI / 180.0f;
        float cosPitch = cosf(pitchRad), sinPitch = sinf(pitchRad);
        float cosYaw = cosf(yawRad), sinYaw = sinf(yawRad);

        float fwdX = cosPitch * cosYaw;
        float fwdY = cosPitch * sinYaw;
        float fwdZ = sinPitch;
        float rightX = -sinYaw;
        float rightY = cosYaw;
        float upX = -sinPitch * cosYaw;
        float upY = -sinPitch * sinYaw;
        float upZ = cosPitch;

        static ESPData localESP[1000];
        int localCount = 0;

        int playerCount = 0, tamedCount = 0, wildCount = 0;

        for (int i = 0; i < Count && localCount < 1000; i++) {
            uintptr_t Actor = *(uintptr_t*)(Actors + i * 8) & 0x00FFFFFFFFFFFFFF;
            if (!Actor || Actor == myPawn) continue;
            if (*(uintptr_t*)Actor == 0 || *(uintptr_t*)Actor < 0x7000000000) continue;

            {
                uint32_t StructID = *(uint32_t*)(Actor + 0xA48);
                uint32_t DinoID1 = *(uint32_t*)(Actor + 0x1A08);
                uint32_t DinoID2 = *(uint32_t*)(Actor + 0x1A0C);
                float maxHp = *(float*)(Actor + 0xC48);
                uintptr_t MyItems = *(uintptr_t*)(Actor + 0x790);
                uintptr_t DroppedName = *(uintptr_t*)(Actor + 0x7E0);
                bool bHasDinoID = (DinoID1 != 0 && DinoID2 != 0);
                bool bHasMaxHealth = (maxHp > 0 && maxHp < 500000);
                if ((StructID > 0 && StructID < 0xFFFFFFFF) && !bHasDinoID && !bHasMaxHealth) continue;
                if (MyItems != 0 || DroppedName != 0) continue;
                if (!bHasDinoID && !bHasMaxHealth) continue;
            }

            float HP = *(float*)(Actor + 0xC44);
            float MaxHP = *(float*)(Actor + 0xC48);
            if ((HP <= 0 || HP > 100000) && (MaxHP <= 0 || MaxHP > 500000)) continue;

            bool bDead = (*(char*)(Actor + 0xBA0) >> 5) & 1;
            if (bDead) continue;

            bool bNPC = (*(char*)(Actor + 0xC70) >> 4) & 1;

            bool bIsPlayer = !bNPC;
            bool bIsTamed = false;
            bool bIsWild = false;
            if (bNPC) {
                int TamingTeamID = *(int*)(Actor + 0x19E8);
                int OwningPlayerID = *(int*)(Actor + 0x19EC);
                bIsTamed = (TamingTeamID != 0 || OwningPlayerID != 0);
                bIsWild = !bIsTamed;
            }

            if (!bNPC && !g_ESP_Players) continue;
            if (bNPC) {
                if (bIsTamed && !g_ESP_TamedDinos) continue;
                if (bIsWild && !g_ESP_WildDinos) continue;
            }

            uintptr_t RC = *(uintptr_t*)(Actor + 0x2C8) & 0x00FFFFFFFFFFFFFF;
            if (!RC) continue;
            float* Pos = (float*)(RC + 0x2D0);
            float posX = Pos[0], posY = Pos[1], posZ = Pos[2];
            if (posX != posX || posY != posY || posZ != posZ) continue;

            float centerZ = bNPC ? posZ + 180.0f : posZ + 90.0f;
            float actorHeight = bNPC ? 360.0f : 180.0f;

            float dx = posX - camX, dy = posY - camY, dz = centerZ - camZ;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz) / 100.0f;

            if (dist <= g_ESP_MaxDistance) {
                if (bIsPlayer) playerCount++;
                else if (bIsTamed) tamedCount++;
                else wildCount++;
            }

            if (dist > g_ESP_MaxDistance) continue;

            float fwd = dx*fwdX + dy*fwdY + dz*fwdZ;
            if (fwd <= 1.0f) continue;

            float rgt = dx*rightX + dy*rightY;
            float upt = dx*upX + dy*upY + dz*upZ;

            float sx = (float)screenWidth/2.0f + (rgt / fwd) * scaleX;
            float sy = (float)screenHeight/2.0f - (upt / fwd) * scaleY;

            float dzLow = (centerZ - actorHeight) - camZ;
            float upLow = dx*upX + dy*upY + dzLow*upZ;
            float syLow = (float)screenHeight/2.0f - (upLow / fwd) * scaleY;

            float boxH = syLow - sy;
            float minH = 12.0f + (camFOV - 90.0f) * 0.1f;
            if (minH < 10.0f) minH = 10.0f;
            if (boxH < minH) syLow = sy + minH;

            float maxH = 500.0f + (camFOV - 90.0f) * 2.0f;
            if (boxH > maxH) syLow = sy + maxH;

            if (syLow < sy) { float tmp = sy; sy = syLow; syLow = tmp; }

            ESPData& data = localESP[localCount];
            data.ScreenX = sx;
            data.ScreenY = sy;
            data.ScreenX2 = sx;
            data.ScreenY2 = syLow;
            data.Distance = dist;
            data.Health = HP;
            data.MaxHealth = MaxHP;
            data.bValid = true;
            data.bIsPlayer = bIsPlayer;
            data.bIsTamed = bIsTamed;


            auto readFString = [](uintptr_t actor, uintptr_t offset, char* out, int maxLen) {
                uintptr_t dataPtr = *(uintptr_t*)(actor + offset);
                int len = *(int*)(actor + offset + 0x8);
                if (!dataPtr || len <= 0 || len > 64) return false;

                int outPos = 0;
                uint16_t* wideStr = (uint16_t*)dataPtr;
                for (int j = 0; j < len && outPos < maxLen - 1; j++) {
                    uint16_t wc = wideStr[j];
                    if (wc == 0) break;

                    if (wc < 0x80) {
                        out[outPos++] = (char)wc;
                    } else if (wc < 0x800) {
                        if (outPos + 1 < maxLen) {
                            out[outPos++] = (char)(0xC0 | (wc >> 6));
                            out[outPos++] = (char)(0x80 | (wc & 0x3F));
                        }
                    } else {
                        if (outPos + 2 < maxLen) {
                            out[outPos++] = (char)(0xE0 | (wc >> 12));
                            out[outPos++] = (char)(0x80 | ((wc >> 6) & 0x3F));
                            out[outPos++] = (char)(0x80 | (wc & 0x3F));
                        }
                    }
                }
                out[outPos] = '\0';
                return (outPos > 0);
            };

            if (bIsPlayer) {
                if (!readFString(Actor, 0x15C8, data.Name, 64)) {
                    strcpy(data.Name, "Player");
                }
            } else {
                if (!readFString(Actor, 0x1538, data.Name, 64)) {
                    if (!readFString(Actor, 0x1528, data.Name, 64)) {
                        strcpy(data.Name, bIsTamed ? "Tamed" : "Wild");
                    }
                }
            }

            localCount++;
        }

        g_ESP_PlayerCount = playerCount;
        g_ESP_TamedCount = tamedCount;
        g_ESP_WildCount = wildCount;
        g_ESP_TotalCount = playerCount + tamedCount + wildCount;

        pthread_mutex_lock(&g_ESPMutex);
        g_ESPCount = localCount;
        memcpy(g_ESPData, localESP, localCount * sizeof(ESPData));
        pthread_mutex_unlock(&g_ESPMutex);

        usleep(16000);
    }
    return nullptr;
}


static void DrawESP() {
    if (!g_ESP_Enabled) return;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    pthread_mutex_lock(&g_ESPMutex);

    for (int i = 0; i < g_ESPCount; i++) {
        if (!g_ESPData[i].bValid) continue;

        float sx = g_ESPData[i].ScreenX;
        float sy = g_ESPData[i].ScreenY;
        float sy2 = g_ESPData[i].ScreenY2;
        float dist = g_ESPData[i].Distance;

        ImU32 color;
        if (g_ESPData[i].bIsPlayer) color = IM_COL32(0, 255, 0, 255);
        else if (g_ESPData[i].bIsTamed) color = IM_COL32(0, 150, 255, 255);
        else color = IM_COL32(255, 0, 0, 255);

        float boxH = sy2 - sy;
        float boxW = boxH * 0.55f;
        if (boxW < 30.0f) boxW = 30.0f;
        if (boxW > 150.0f) boxW = 150.0f;

        if (g_ESPData[i].bIsPlayer) {
            float boxTop = sy, boxBottom = sy2;
            if ((boxBottom - boxTop) < 25.0f) boxBottom = boxTop + 25.0f;
            float boxX1 = sx - boxW / 2.0f, boxX2 = sx + boxW / 2.0f;

            if (g_ESP_Snaplines) draw->AddLine(ImVec2(screenWidth/2.0f, screenHeight), ImVec2(sx, boxBottom), color, 3.0f);
            if (g_ESP_Boxes) {
                draw->AddRect(ImVec2(boxX1+1, boxTop+1), ImVec2(boxX2+1, boxBottom+1), IM_COL32(0,0,0,160), 0,0,4.0f);
                draw->AddRect(ImVec2(boxX1, boxTop), ImVec2(boxX2, boxBottom), color, 0,0,3.0f);
            }

            float topY = boxTop - 6.0f;
            
            if (g_ESP_Names) {
                topY -= 18.0f;
                ImU32 nameColor = IM_COL32(0, 255, 0, 255); 
                
                draw->AddText(ImVec2(sx-45+1, topY), nameColor, g_ESPData[i].Name);
                draw->AddText(ImVec2(sx-45-1, topY), nameColor, g_ESPData[i].Name);
                draw->AddText(ImVec2(sx-45, topY+1), nameColor, g_ESPData[i].Name);
                draw->AddText(ImVec2(sx-45, topY-1), nameColor, g_ESPData[i].Name);
                draw->AddText(ImVec2(sx-45, topY), IM_COL32(255, 255, 255, 255), g_ESPData[i].Name);
            }
            if (g_ESP_Health) {
                float barH = 7.0f; topY -= barH + 3.0f;
                float hpPercent = g_ESPData[i].Health / g_ESPData[i].MaxHealth;
                if (hpPercent > 1.0f) hpPercent = 1.0f; if (hpPercent < 0.0f) hpPercent = 0.0f;
                draw->AddRectFilled(ImVec2(boxX1, topY), ImVec2(boxX2, topY+barH), IM_COL32(15,15,15,240));
                float hw = boxW * hpPercent; if (hw < 1.0f) hw = 1.0f;
                ImU32 hpCol = (hpPercent<0.3f) ? IM_COL32(255,50,50,255) : (hpPercent<0.6f) ? IM_COL32(255,200,0,255) : IM_COL32(0,255,0,255);
                draw->AddRectFilled(ImVec2(boxX1, topY), ImVec2(boxX1+hw, topY+barH), hpCol);
                draw->AddRect(ImVec2(boxX1, topY), ImVec2(boxX2, topY+barH), IM_COL32(0,0,0,255), 0,0,1.5f);
            }
            if (g_ESP_Distance) { char b[32]; snprintf(b,32,"[%.0fm]",dist); draw->AddText(ImVec2(sx-25, boxBottom+4.0f), IM_COL32(255,255,200,255), b); }
        } else {
            if (g_ESP_Snaplines) draw->AddLine(ImVec2(screenWidth/2.0f, screenHeight), ImVec2(sx, sy2), color, 3.0f);

            float topY = sy - 6.0f;
            
            if (g_ESP_Names) {
                topY -= 18.0f;
                ImU32 nameColor;
                if (g_ESPData[i].bIsTamed) {
                    nameColor = IM_COL32(0, 150, 255, 255); 
                } else {
                    nameColor = IM_COL32(255, 80, 80, 255); 
                }
               
                draw->AddText(ImVec2(sx-45+1, topY), nameColor, g_ESPData[i].Name);
                draw->AddText(ImVec2(sx-45-1, topY), nameColor, g_ESPData[i].Name);
                draw->AddText(ImVec2(sx-45, topY+1), nameColor, g_ESPData[i].Name);
                draw->AddText(ImVec2(sx-45, topY-1), nameColor, g_ESPData[i].Name);
               
                draw->AddText(ImVec2(sx-45, topY), IM_COL32(255, 255, 255, 255), g_ESPData[i].Name);
            }
            if (g_ESP_Health) {
                float barH = 7.0f, barW = boxW, barX = sx - barW/2;
                topY -= barH + 3.0f;
                float hpPercent = g_ESPData[i].Health / g_ESPData[i].MaxHealth;
                if (hpPercent > 1.0f) hpPercent = 1.0f; if (hpPercent < 0.0f) hpPercent = 0.0f;
                draw->AddRectFilled(ImVec2(barX, topY), ImVec2(barX+barW, topY+barH), IM_COL32(15,15,15,240));
                float hw = barW * hpPercent; if (hw < 1.0f) hw = 1.0f;
                ImU32 hpCol = (hpPercent<0.3f) ? IM_COL32(255,50,50,255) : (hpPercent<0.6f) ? IM_COL32(255,200,0,255) : IM_COL32(0,255,0,255);
                draw->AddRectFilled(ImVec2(barX, topY), ImVec2(barX+hw, topY+barH), hpCol);
                draw->AddRect(ImVec2(barX, topY), ImVec2(barX+barW, topY+barH), IM_COL32(0,0,0,255), 0,0,1.5f);
            }
            if (g_ESP_Distance) { char b[32]; snprintf(b,32,"[%.0fm]",dist); draw->AddText(ImVec2(sx-25, sy2+4.0f), IM_COL32(255,255,200,255), b); }
        }
    }

    
    if (g_ESP_TotalCount > 0) {
        char countBuf[256];
        snprintf(countBuf, sizeof(countBuf), "Total: %d  |  Players: %d  |  Tamed: %d  |  Wild: %d",
                 g_ESP_TotalCount, g_ESP_PlayerCount, g_ESP_TamedCount, g_ESP_WildCount);
        float textWidth = strlen(countBuf) * 8.0f;
        float countX = (screenWidth - textWidth) / 2.0f;
        float countY = 10.0f;
        draw->AddRectFilled(ImVec2(countX-10, countY-2), ImVec2(countX+textWidth+10, countY+22), IM_COL32(0,0,0,180));
        draw->AddRect(ImVec2(countX-10, countY-2), ImVec2(countX+textWidth+10, countY+22), IM_COL32(100,100,100,255), 0,0,1.5f);
        draw->AddText(ImVec2(countX, countY), IM_COL32(255,255,255,255), countBuf);
    }

    pthread_mutex_unlock(&g_ESPMutex);
}
