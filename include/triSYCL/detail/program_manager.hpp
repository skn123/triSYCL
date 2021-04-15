#ifndef TRISYCL_SYCL_DETAIL_PROGRAM_MANAGER_HPP
#define TRISYCL_SYCL_DETAIL_PROGRAM_MANAGER_HPP

#include <cassert>
#include <iostream>
#include <string>
#include <utility>
#include <fstream>

#include "triSYCL/detail/debug.hpp"
#include "triSYCL/detail/singleton.hpp"

/** \file The minimum required functions for registering a binary
    using the triSYCL/Intel SYCL frontend, so that the compilation flow stays
    happy without too many alterations for now (the offloader needs these
    defined, even if they do nothing).

    The full implementations for this will require lifting existing
    triSYCL/Intel SYCL frontend code from triSYCL/Intel SYCL runtime's pi.h
    and program_manager.cpp/h files. There is a lot that is in the other SYCL
    runtime that's not here, so if you need functionality go look in the stated
    header and source files inside of: https://github.com/triSYCL/sycl

    This offloader and PI interface is unfortunately mostly a raw C Library and
    these functions and a lot of other functionality must be marked with extern
    C for now.

    Andrew point Gozillon at yahoo point com

    This file is distributed under the University of Illinois Open Source
    License. See LICENSE.TXT for details.
*/

// +++ Offload Wrapper Types, they must structurally match those defined by the
// clang-offload-wrapper if you use them but the names don't need to be
// equivalent they also all contain Intel SYCL pi.h equivalents
// (different naming convention) {

/// Offload entry point, possibly not required by us and it's not used by Intel
/// SYCL. It matches the structure __tgt_offload_entry generated by the
/// clang-offload-wrapper and is named _pi_offload_entry in the SYCL
/// PI interface
///
/// TODO: If this isn't required it's feasible to make it an opaque void type
/// like SYCL does so it remains compatible with OpenMP and the other SYCL
/// implementations e.g:
///
/// typedef void * __tgt_offload_entry;
///
struct __sycl_offload_entry {
  void *addr;
  char *name;
  size_t size;
  int32_t flags;
  int32_t reserved;
};

/// This struct is a record of the device binary information.
/// It must match the __tgt_device_image structure generated by the
/// clang-offload-wrapper tool when their Version fields match.
///
/// It's named pi_device_binary_struct in Intel SYCL's PI interface.
struct __sycl_device_image {
  /// version of this structure - for backward compatibility;
  /// all modifications which change order/type/offsets of existing fields
  /// should increment the version.
  uint16_t Version;
  /// the kind of offload model the image employs.
  uint8_t OffloadKind;
  /// format of the image data - SPIRV, LLVMIR bitcode,...
  uint8_t Format;
  /// null-terminated string representation of the device's target
  /// architecture
  const char *DeviceTargetSpec;
  /// a null-terminated string; target- and compiler-specific options
  /// which are suggested to use to "compile" program at runtime
  const char *CompileOptions;
  /// a null-terminated string; target- and compiler-specific options
  /// which are suggested to use to "link" program at runtime
  const char *LinkOptions;
  /// Pointer to the manifest data start
  const unsigned char *ManifestStart;
  /// Pointer to the manifest data end
  const unsigned char *ManifestEnd;
  /// Pointer to the device binary image start
  const unsigned char *ImageStart;
  /// Pointer to the device binary image end
  const unsigned char *ImageEnd;
  /// the entry table
  __sycl_offload_entry *EntriesBegin;
  __sycl_offload_entry *EntriesEnd;

  /// Unused for now.
  void* PropertySetsBegin;
  void* PropertySetsEnd;
};

/// This must match the __tgt_bin_desc of the clang-offload-wrapper
///
/// In the Intel SYCL PI interface it's named pi_device_binaries_struct
struct __sycl_bin_desc {
  /// version of this structure - for backward compatibility;
  /// all modifications which change order/type/offsets of existing fields
  /// should increment the version.
  uint16_t Version;
  uint16_t NumDeviceImages;
  __sycl_device_image *DeviceImages;
  /// the offload entry table
  __sycl_offload_entry *HostEntriesBegin;
  __sycl_offload_entry *HostEntriesEnd;
};

// +++ }


namespace trisycl::detail {

  /** Singleton style program manager that's job is to hold all of the compiler
   registered binaries and manage them. It's bare-bones and based on the Intel
   SYCL implementation for now so it's quite feasible to deviate from it if
   desired, the only requirement is that something stores the device_images
   passed to the __tgt_register_lib function and that all of the used offload
   wrapper types remain identical to the offload wrappers (name can be
   different, but if the data layout is incorrect you're probably going to
   have a bad time).

   Image is equal to binary in the current wording of the documentation/naming,
   using the words interchangeably just now.

   \todo 1) Locks for addImages and other components that are not thread safe
   \todo 2) Proper image cleanup/release, there is none at the moment so this
  					 is pretty much one big memory leak
   \todo 3) Maybe make the structure names a little better than sycl_*
   \todo 4) Map<Name, Binary> instead of vector<pair<Name, Binary>> for storing
  		  		 copies of image data, more efficient than iterating over all images
   \todo 5) This could also just be part of the array.hpp class, but it could
  					 also be more generic and used to store images for other types e.g
  					 arm32, fpga etc.
   \todo 6) Verify ELF Magic of image on registration so we can catch invalid
            binaries early
*/

class program_manager : public detail::singleton<program_manager> {
private:
  /// A work-around to a work-around, the AI Engine runtime does some
  /// manipulation of the elf binary in memory to work through a chess compiler
  /// bug, so we need a copy of it as the original buffer containing each image
  /// is read-only/const (not because of the const pointer defined in the
  /// structure, but because the section inside the host binary that the device
  /// binary is allocated in is a read-only section).
  ///
  /// We also need to make a pairing between the binary and its name so we can
  /// retrieve it via integration header.
  std::vector<std::pair</*Name*/std::string, /*Binary*/std::string_view>> image_list;

public:
  static bool isELFMagic(const char *BinStart) {
    return BinStart[0] == 0x7f && BinStart[1] == 'E' && BinStart[2] == 'L' &&
           BinStart[3] == 'F';
  }
  /// This in theory loads all of the images for a module, a module may not
  /// necessarily a translation unit it may also contain kernels from
  /// other TUs. But this requires a little more investigation as we have no
  /// test that spans multiple TUs yet! See Intel SYCL LLVM Pull:
  /// https://github.com/intel/llvm/pull/631
  void addImages(__sycl_bin_desc* desc) {
    TRISYCL_DUMP_T("Number of Device Images being registered from module: "
                   << desc->NumDeviceImages << "\n");

    for (int i = 0; i < desc->NumDeviceImages; ++i) {
      __sycl_device_image* img = &(desc->DeviceImages[i]);

      TRISYCL_DUMP_T("Registering Image " << i << " Bin size "
                     << ((intptr_t)img->ImageEnd - (intptr_t)img->ImageStart)
                     << " Start Address of RO image section: "
                     << (void *)img->ImageStart
                     << " End Address of RO image section: "
                     << (void *)img->ImageEnd << "\n");

#ifdef TRISYCL_DEBUG_IMAGE
      /// Dump image on registration, used to make sure the initial binary image
      /// is correct before we do anything with it in our runtime
      image_dump(img, "aie" + std::string(Img->BuildOptions) + ".elf");
#endif

      /// Images are not directly ELF binaries, they can contain multiple ELF
      /// binaries, and also contain their name. The images are built inside
      /// sycl-chess.
      /// The current format of each kernels is:
      /// name_of_kernel\n
      /// size_of_kernel(in characters not binary form)\n
      /// the binary
      /// next kernel or end of file.
      /// It is assumed in the following code that the format is correct, there
      /// is no attempt to detect or correct invalid formats.
      const char* ptr = reinterpret_cast<const char*>(img->ImageStart);
      /// Loop until we have seen the hole image.
      while (reinterpret_cast<uintptr_t>(ptr) <
             reinterpret_cast<uintptr_t>(img->ImageEnd)) {
        unsigned next_size = 0;
        while (ptr[next_size] != '\n')
          next_size++;
        std::string name(ptr, next_size);
        ptr += next_size + 1;
        next_size = 0;
        while (ptr[next_size] != '\n')
          next_size++;
        unsigned bin_size = std::stoi(std::string(ptr, next_size));
        ptr += next_size + 1;
        next_size = 0;
        std::string_view bin(ptr, bin_size);
        ptr += bin_size;
        TRISYCL_DUMP_T("Loading Name: " << name << " Size: " << bin_size
                                        << " Magic: \"" << bin.substr(0, 4)
                                        << "\" IsElf: "
                                        << isELFMagic(bin.data()));
        /// We piggyback off of the BuildOptions parameter of the device_image
        /// description as it's unused in our case and it allows us to avoid
        /// altering the ClangOffloadWrappers types which causes them to risk
        /// incompatibility with Intel's SYCL implementation and OpenMP/HIP
        image_list.emplace_back(std::move(name), bin);
      }
      assert(reinterpret_cast<uintptr_t>(ptr) ==
                 reinterpret_cast<uintptr_t>(img->ImageEnd) &&
             "invalid Image format");
    }
  }

  // Simple test function to dump an image to file
  void image_dump(__sycl_device_image * img, const std::string& filename) {
    std::ofstream F{filename, std::ios::binary};

    if (!F.is_open()) {
      std::cerr << "File for image dump could not be opened \n";
      return;
    }

    size_t ImgSize = static_cast<size_t>((intptr_t)img->ImageEnd
                                       - (intptr_t)img->ImageStart);
    F.write(reinterpret_cast<const char *>(img->ImageStart), ImgSize);
    F.close();
  }

  // Simple test function to dump an image to file
  void image_dump(const std::string& img, const std::string& filename) {
    std::ofstream F{filename, std::ios::binary};

    if (!F.is_open()) {
      std::cerr << "File for image dump could not be opened \n";
      return;
    }

    F.write(img.data(), img.size());
    F.close();
  }

  /// A write-able copy of our binary image, mainly so the AI Engine runtime can
  /// do some manipulation to workaround some chess problems.
  ///
  /// Return by index, only relevant really if you're testing for now and know
  /// the exact location in the vector that the image resides.
  std::string_view get_image(const unsigned int index) {
    return std::get<1>(image_list.at(index));
  }

  /// A write-able copy of our binary image, mainly so the AI Engine runtime can
  /// do some manipulation to workaround some chess problems.
  ///
  /// Return by kernel name, used to map integration header names to offloaded
  /// binary names
  std::string_view get_image(const std::string& kernelName) {
    for (auto image : image_list)
      if (std::get<0>(image) == kernelName) {
        return std::get<1>(image);
      }
    return {};
  }
};

}

/// These need to be marked with C Linkage because the offloader will not mangle
/// the names and mangling them is not a good idea as the functions can be
/// compiled by the host compiler, which can vary and then the mangled name of
/// the function will vary dependent on the chosen host compiler (
/// Itanium/Microsoft etc).
///
/// It should never be inlined, even though extern C linkage is usually never
/// inlined by default it's worth being explicit as a lot of triSYCL ends up
/// with the inline keyword over time as we are very inline trigger happy..
///
/// The most important aspect is that it needs to be defined as weak, this is a
/// C function in a header file that cannot be inlined and cannot be defined
/// with internal linkage via static. It will get defined multiple times when
/// linking across translation units, without weak linkage telling it this
/// function is arbitrarily replaceable you'll violate the ODR. If this gets
/// ported to MSVC __declspec(selectany) should do what weak is intended for in
/// this scenario
extern "C" {

  // +++ Entry points referenced by the offload wrapper object {

  /// Executed as a part of current module's (.exe, .dll) static initialization.
  /// Registers device executable images with the runtime.
  /// Slightly modified variation of the Intel SYCL program_manager
  /// implementation
  /// TODO: Make barebones singleton program manager class that keeps track of
  /// our binaries...
  void __attribute__((noinline, weak))
    __sycl_register_lib(__sycl_bin_desc* desc)  {
    trisycl::detail::program_manager::instance()->addImages(desc);
  }

  /// Executed as a part of current module's (.exe, .dll) static
  /// de-initialization.
  /// Unregisters device executable images with the runtime.
  /// \TODO: Implement __tgt_unregister_lib for ACAP++ to enable binary
  /// management, although it isn't implemented in Intel SYCL as of the time of
  /// writing this.
  void __attribute__((noinline, weak))
    __sycl_unregister_lib(__sycl_bin_desc* desc) {}

  // +++ }

}

/*
    # Some Emacs stuff:
    ### Local Variables:
    ### ispell-local-dictionary: "american"
    ### eval: (flyspell-prog-mode)
    ### End:
*/

#endif // TRISYCL_SYCL_DETAIL_PROGRAM_MANAGER_HPP
