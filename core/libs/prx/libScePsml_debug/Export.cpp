#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr std::int32_t SCE_PSML_ERROR_NOT_INITIALIZED = static_cast<std::int32_t>(0x8A810001);

}

extern "C" {

std::int32_t APS5_VABI scePsmlMfsrGetContextBufferRequirement1100(void* requirement, const void* param) {
 (void)requirement;
 (void)param;
 return SCE_PSML_ERROR_NOT_INITIALIZED;
}

std::int32_t APS5_VABI scePsmlMfsrCreateContext1100(void** context, const void* param) {
 (void)context;
 (void)param;
 return SCE_PSML_ERROR_NOT_INITIALIZED;
}

std::int32_t APS5_VABI scePsmlMfsrGetDispatchMfsrPacket1100(void* context, void* commandBuffer, const void* param) {
 (void)context;
 (void)commandBuffer;
 (void)param;
 return SCE_PSML_ERROR_NOT_INITIALIZED;
}

int APS5_VABI scePsmlMfsrGetSharedResourcesInitRequirement() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePsmlMfsrInit() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePsmlMfsrGetDispatchMfsrPacketSizeInDwords() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePsmlMfsrGetContextBufferRequirement800M3_2() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePsmlMfsrSelectConfig() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePsmlMfsrGetMipmapBias() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePsmlMfsrRequestCapture() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePsmlMfsrReleaseContext() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePsmlMfsrIsCaptureInProgress() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePsmlMfsrGetDispatchMfsrPacket900() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePsmlMfsrCreateSharedResources() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePsmlMfsrCreateContext800M3_2() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePsmlMfsrReleaseSharedResources() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

APS5_EXPORT("0GAw7SmkwII", scePsmlMfsr2Unknown_0GAw7SmkwII);
int APS5_VABI scePsmlMfsr2Unknown_0GAw7SmkwII(void) {
    NotImplemented_nid_no_patch("0GAw7SmkwII");
    return 0;
}

APS5_EXPORT("1ic5q-kdOsc", scePsmlMfsr2Unknown_1ic5q_MkdOsc);
int APS5_VABI scePsmlMfsr2Unknown_1ic5q_MkdOsc(void) {
    NotImplemented_nid_no_patch("1ic5q-kdOsc");
    return 0;
}

APS5_EXPORT("5z2gBlqxJ+0", scePsmlMfsr2Unknown_5z2gBlqxJ_P0);
int APS5_VABI scePsmlMfsr2Unknown_5z2gBlqxJ_P0(void) {
    NotImplemented_nid_no_patch("5z2gBlqxJ+0");
    return 0;
}

APS5_EXPORT("Kyy1baXgaVU", scePsmlMfsr2Unknown_Kyy1baXgaVU);
int APS5_VABI scePsmlMfsr2Unknown_Kyy1baXgaVU(void) {
    NotImplemented_nid_no_patch("Kyy1baXgaVU");
    return 0;
}

APS5_EXPORT("ZLL31lzzxr4", scePsmlMfsr2Unknown_ZLL31lzzxr4);
int APS5_VABI scePsmlMfsr2Unknown_ZLL31lzzxr4(void) {
    NotImplemented_nid_no_patch("ZLL31lzzxr4");
    return 0;
}

APS5_EXPORT("gMduXCLYrNg", scePsmlMfsr2Unknown_gMduXCLYrNg);
int APS5_VABI scePsmlMfsr2Unknown_gMduXCLYrNg(void) {
    NotImplemented_nid_no_patch("gMduXCLYrNg");
    return 0;
}

APS5_EXPORT("lrJwpLjKXRc", scePsmlMfsr2Unknown_lrJwpLjKXRc);
int APS5_VABI scePsmlMfsr2Unknown_lrJwpLjKXRc(void) {
    NotImplemented_nid_no_patch("lrJwpLjKXRc");
    return 0;
}

APS5_EXPORT("m9JLPc3wOQw", scePsmlMfsr2Unknown_m9JLPc3wOQw);
int APS5_VABI scePsmlMfsr2Unknown_m9JLPc3wOQw(void) {
    NotImplemented_nid_no_patch("m9JLPc3wOQw");
    return 0;
}

APS5_EXPORT("o+NM86gEwFE", scePsmlMfsr2Unknown_o_PNM86gEwFE);
int APS5_VABI scePsmlMfsr2Unknown_o_PNM86gEwFE(void) {
    NotImplemented_nid_no_patch("o+NM86gEwFE");
    return 0;
}

}
