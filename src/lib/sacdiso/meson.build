sacdiso = static_library(
  'sacdiso',
  'sacd_disc.cpp',
  'sacd_dsdiff.cpp',
  'sacd_media.cpp',
  'sacd_metabase.cpp',
  'scarletbook.cpp',
  'libdstdec/ACData.cpp',
  'libdstdec/CodedTable.cpp',
  'libdstdec/DSTFramework.cpp',
  'libdstdec/FrameReader.cpp',
  'libdstdec/StrData.cpp',
  'libdstdec/binding/dst_decoder_mpd.cpp',
  'libdstdec/decoder/DSTDecoder.cpp',
  include_directories: inc,
  dependencies: [
    gcrypt_dep,
  ],
)

sacdiso_dep = declare_dependency(
  link_with: sacdiso,
)
