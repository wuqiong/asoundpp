#include "FLAC/stream_decoder.h"

#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <boost/cstdint.hpp>
#include <boost/make_shared.hpp>
#include <boost/noncopyable.hpp>
#include <boost/optional.hpp>

#include "input_stream.h"

namespace
{

struct decoder : input_stream
{
   decoder(std::string const& filename)
      : dec(FLAC__stream_decoder_new())
      , current_pos()
   {
      if (dec == NULL)
         throw std::runtime_error("error allocating flac decoder");

      (void)FLAC__stream_decoder_set_md5_checking(dec, true);

      FLAC__StreamDecoderInitStatus init_status = FLAC__stream_decoder_init_file(dec, filename.c_str(), &do_write, &do_metadata, &do_error, this);
      if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
      {
         if (init_status == FLAC__STREAM_DECODER_INIT_STATUS_ERROR_OPENING_FILE)
         {
            std::stringstream ss;
            ss << "unable to initialize flac decoder (error opening file \"";
            ss << filename;
            ss << "\")";

            throw std::runtime_error(ss.str());
         }
         else
         {
            std::stringstream ss;
            ss << "unable to initialize flac decoder (";
            ss << FLAC__StreamDecoderInitStatusString[init_status];
            ss << ")";

            throw std::runtime_error(ss.str());
         }
      }

      int res = FLAC__stream_decoder_process_until_end_of_metadata(dec);
      if (res == 0)
         throw std::runtime_error("unable to read metadata from flac file");

      check_last_error_and_throw();

      if (!metadata_)
         throw std::runtime_error("libflac read metadata successfully, but no metadata callback was called");
   }

   ~decoder()
   {
      FLAC__stream_decoder_delete(dec);
   }

   format get_format()
   {
      return metadata_->first;
   }

   size_t number_of_frames()
   {
      return metadata_->second;
   }

   void seek(size_t frame_n)
   {
      check_last_error_and_throw();

      current_pos = frame_n;

      written_data.clear();
      int res = FLAC__stream_decoder_seek_absolute(dec, frame_n); // TODO: handle error and reset (!) decoder
      if (res == 0)
      {
         set_last_error("seek failed");
         check_last_error_and_throw();
      }
   }

   size_t get_position()
   {
      return current_pos;
   }

   void read(void* buf, size_t number_of_frames)
   {
      check_last_error_and_throw();

      size_t number_of_bytes = number_of_frames * get_format().frame_size();

      for (;;)
      {
         if (written_data.size() >= number_of_bytes)
         {
            std::copy(written_data.begin(), written_data.begin() + number_of_bytes, static_cast<char*>(buf));
            written_data.erase(written_data.begin(), written_data.begin() + number_of_bytes);

            current_pos += number_of_frames;
            return;
         }

         int res = FLAC__stream_decoder_process_single(dec);
         if (res == 0)
            set_last_error("unknown error"); // set if isn't set already

         check_last_error_and_throw();
      }
   }

private:
   static FLAC__StreamDecoderWriteStatus do_write(const FLAC__StreamDecoder*,
                                                  const FLAC__Frame *frame,
                                                  const FLAC__int32 * const buffer[],
                                                  void *client_data)
   {
      decoder* d = static_cast<decoder*>(client_data);

      try
      {
         for (unsigned i = 0; i != frame->header.blocksize; ++i)
         {
            d->write_i16(buffer[0][i]);
            d->write_i16(buffer[1][i]);
         }
      }
      catch (std::exception const& e)
      {
         std::stringstream ss;
         ss << "error at buffer processing: " << e.what();
         d->set_last_error(ss.str());
         return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
      }

      return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
   }

   static void do_metadata(const FLAC__StreamDecoder*, const FLAC__StreamMetadata *fmetadata, void *client_data)
   {
      decoder* d = static_cast<decoder*>(client_data);

      if (d->metadata_)
      {
         d->set_last_error("duplicate metadata");
         return;
      }

      if (fmetadata->data.stream_info.bits_per_sample != 16)
      {
         d->set_last_error("only 16 bits per sample is supported");
         return;
      }

      format fmt;
      size_t number_of_frames  = fmetadata->data.stream_info.total_samples;
      fmt.sample_rate          = fmetadata->data.stream_info.sample_rate;
      fmt.channels             = fmetadata->data.stream_info.channels;
      fmt.fmt                  = SND_PCM_FORMAT_S16_LE;

      d->metadata_ = std::make_pair(fmt, number_of_frames);
   }

   static void do_error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status, void* client_data)
   {
      decoder* d = static_cast<decoder*>(client_data);

      std::stringstream ss;
      ss << "flac decoding error: " << FLAC__StreamDecoderErrorStatusString[status];
      d->set_last_error(ss.str());
   }

   void write_i16(boost::int16_t v)
   {
      written_data.push_back(v);
      written_data.push_back(v >> 8);
   }

   void set_last_error(std::string const& msg)
   {
      if (last_error_.empty())
         last_error_ = msg;
   }

   void check_last_error_and_throw()
   {
      if (last_error_.empty())
         return;

      std::stringstream ss;
      ss << "flac decoding error: " << last_error_;
      throw std::runtime_error(ss.str());
   }

private:
   FLAC__StreamDecoder*      dec;
   std::string               last_error_;
   boost::optional<std::pair<format, size_t> > metadata_;
   std::vector<char>         written_data;
   size_t                    current_pos;
};

}

input_stream_sp open_flac_file(std::string const& filename)
{
   return boost::make_shared<decoder>(filename);
}
