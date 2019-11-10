
#include <math.h>

#include <logging.hh>

#include <fec.hh>

inline double cur_time() {
  struct timeval t;
  gettimeofday(&t, 0);
  return double(t.tv_sec) + double(t.tv_usec) * 1e-6;
}

/*******************************************************************************
 * FECEncoder
 ******************************************************************************/

FECEncoder::FECEncoder(uint8_t num_blocks, uint8_t num_fec_blocks, uint16_t max_block_size,
		       uint8_t start_seq_num) :
  m_num_blocks(num_blocks), m_num_fec_blocks(num_fec_blocks), m_seq_num(start_seq_num) {
  // Ensure that the FEC library is initialized
  // This may not work with multiple threads!
  fec_init();
}

// Allocate and initialize the next data block.
std::shared_ptr<FECBlock> FECEncoder::get_next_block(uint16_t length) {
  return std::shared_ptr<FECBlock>(new FECBlock(m_seq_num, m_in_blocks.size(), m_num_blocks,
						m_num_fec_blocks, length));
}

// Add an incoming data block to be encoded
void FECEncoder::add_block(std::shared_ptr<FECBlock> block) {
  FECHeader *h = block->header();
  h->block = m_in_blocks.size();

  // Just output the block if we're not actually encoding.
  if ((m_num_fec_blocks == 0) || (m_num_blocks == 0)) {
    m_out_blocks.push(block);
    ++m_seq_num;
    if(m_seq_num == 0) {
      ++m_seq_num;
    }
  } else {
    m_in_blocks.push_back(block);

    // Calculate the FEC blocks when we've received enough blocks.
    if ((m_num_fec_blocks > 0) && (h->block == (m_num_blocks - 1))) {
      encode_blocks();
    }
  }
}

// Retrieve the next data/fec block
std::shared_ptr<FECBlock> FECEncoder::get_block() {
  if (m_out_blocks.empty()) {
    return std::shared_ptr<FECBlock>();
  }
  std::shared_ptr<FECBlock> ret = m_out_blocks.front();
  m_out_blocks.pop();
  return ret;
}

// Complete the sequence with the current set of blocks
void FECEncoder::flush() {
  encode_blocks();
}

void FECEncoder::encode_blocks() {
  uint8_t num_blocks = m_in_blocks.size();
  if (num_blocks == 0) {
    return;
  }

  // Create the FEC arrays of pointers to the data blocks.
  std::vector<uint8_t*> data_blocks(m_num_blocks);
  uint16_t block_size = 0;
  for (uint8_t i = 0; i < num_blocks; ++i) {
    std::shared_ptr<FECBlock> block = m_in_blocks[i];
    data_blocks[i] = block->fec_data();
    block_size = std::max(block_size, static_cast<uint16_t>(block->header()->length + 2));
    block->header()->n_blocks = num_blocks;
    m_out_blocks.push(block);
  }

  // Create the output FEC blocks
  std::vector<uint8_t*> fec_blocks(m_num_fec_blocks);
  for (uint8_t i = 0; i < m_num_fec_blocks; ++i) {
    std::shared_ptr<FECBlock> block(new FECBlock(m_seq_num, num_blocks + i, num_blocks,
						 m_num_fec_blocks, block_size - 2));
    fec_blocks[i] = block->fec_data();
    block->pkt_length(block_size + sizeof(FECHeader) - 2);
    m_out_blocks.push(block);
  }

  // Encode the blocks.
  fec_encode(block_size, data_blocks.data(), num_blocks, fec_blocks.data(), m_num_fec_blocks);

  // Prepare for the next set of blocks.
  ++m_seq_num;
  if(m_seq_num == 0) {
    ++m_seq_num;
  }
  m_in_blocks.clear();
}


/*******************************************************************************
 * FECDecoderEncoder
 ******************************************************************************/

FECDecoder::FECDecoder() : m_block_size(0) {
  fec_init();
}

void FECDecoder::add_block(const uint8_t *buf, uint16_t block_length) {
  std::shared_ptr<FECBlock> blk(new FECBlock(buf, block_length));
  const FECHeader &h = *blk->header();
  uint8_t n_blocks = h.n_blocks;
  uint8_t n_fec_blocks = h.n_fec_blocks;
  FECHeader &ph = m_prev_header;
  uint16_t unrolled_prev_seq = static_cast<uint16_t>(ph.seq_num);
  uint16_t unrolled_seq = static_cast<uint16_t>(h.seq_num);
  if (unrolled_prev_seq > unrolled_seq) {
    unrolled_seq += 256;
  }
  ++m_stats.total_packets;
  m_stats.bytes += block_length;

  // Are we actively processing a block?
  if (m_block_size != 0) {

    // Did we reach the end of a sequence without getting enough blocks?
    if (unrolled_prev_seq != unrolled_seq) {

      // If se get a (unrolled) sequence number that is less than the previous sequence number
      // we've obviously lost sync.
      if (unrolled_seq < unrolled_prev_seq) {
	++m_stats.lost_sync;
      } else {

	// Calculate how many packets we dropped with this break in the sequence.
	m_stats.dropped_blocks += unrolled_seq - unrolled_prev_seq;

	// Calculate how many packets we dropped.
	uint64_t pbn = unrolled_prev_seq * static_cast<uint64_t>(n_blocks + n_fec_blocks) + ph.block;
	uint64_t bn = unrolled_seq * static_cast<uint64_t>(n_blocks + n_fec_blocks) + h.block;
	if (pbn < bn) {
	  m_stats.dropped_packets += bn - pbn;
	}
      }

      // Reset the sequence.
      m_block_size = 0;
      m_blocks.clear();
      m_fec_blocks.clear();

    } else if(h.block <= ph.block) {

      // This shouldn't happen.
      ++m_stats.dropped_packets;
    } else {

      // Record any dropped packets from the last packet.
      m_stats.dropped_packets += (h.block - ph.block) - 1;
    }

  } else if (unrolled_prev_seq == unrolled_seq) {

    // Are we skipping past the end if the current block that has been completed?
    ph = h;
    return;
  }
  ph = h;

  // Record any packets we might have skipped at the beginning of the sequence.
  if (m_block_size == 0) {
    m_stats.dropped_packets += h.block;
  }

  // Just release the block of FEC is not being performed on this channel.
  if ((n_blocks == 0) || (n_fec_blocks == 0)) {
    m_out_blocks.push(blk);
    return;
  }

  // The current block size is equal to the block size of the largest block.
  m_block_size = std::max(m_block_size, blk->block_size());

  // Is this a data block or FEC block?
  if (blk->is_data_block()) {

    // Add this block to the list of blocks.
    m_blocks.push_back(blk);

    // Release the block if we don't have a gap.
    if ((m_blocks.size() - 1) == h.block) {
      m_out_blocks.push(blk);
    }

    // Have we reached the end of the data blocks without dropping a packet?
    if (m_blocks.size() == n_blocks) {
      m_block_size = 0;
      m_blocks.clear();
      m_fec_blocks.clear();
      ++m_stats.total_blocks;
    }

  } else {

    // Add this block to the list of FEC blocks.
    m_fec_blocks.push_back(blk);

    // Decode once we've received anough blocks + FEC blocks and have dropped a block.
    if ((m_blocks.size() + m_fec_blocks.size()) == n_blocks) {

      // Decode the sequence
      decode();

      // Start waiting for blocks from the next packet.
      m_block_size = 0;
      m_blocks.clear();
      m_fec_blocks.clear();
      ++m_stats.total_blocks;
    }
  }
}

void FECDecoder::decode() {
  if (m_blocks.size() == 0) {
    return;
  }
  const FECHeader &h = *m_blocks[0]->header();
  uint8_t n_blocks = h.n_blocks;
  uint8_t n_fec_blocks = h.n_fec_blocks;

  // Sanity check the number of blocks
  if (n_blocks > (m_blocks.size() + m_fec_blocks.size())) {
    ++m_stats.lost_sync;
    return;
  }
  if ((n_blocks == 0) || (n_fec_blocks == 0)) {
    return;
  }

  // Create the vector of data blocks.
  std::vector<uint8_t*> block_ptrs(n_blocks, 0);
  std::vector<std::shared_ptr<FECBlock> > blocks(n_blocks);
  for (auto block : m_blocks) {
    blocks[block->header()->block] = block;
    block_ptrs[block->header()->block] = block->fec_data();
  }

  // Create the erased blocks array
  std::vector<std::shared_ptr<FECBlock> > fec_blocks;
  std::vector<unsigned int> erased_block_idxs;
  for (size_t i = 0; i < h.n_blocks; ++i) {
    if (!block_ptrs[i]) {
      std::shared_ptr<FECBlock> blk(new FECBlock(h, m_block_size));
      erased_block_idxs.push_back(i);
      fec_blocks.push_back(blk);
      blocks[i] = blk;
      block_ptrs[i] = blk->fec_data();
    }
  }

  // Create the FEC blocks array
  std::vector<uint8_t*> fec_block_ptrs;
  std::vector<unsigned int> fec_block_idxs;
  for (auto block : m_fec_blocks) {
    uint8_t fec_block_idx = block->header()->block - block->header()->n_blocks;
    fec_block_ptrs.push_back(block->fec_data());
    fec_block_idxs.push_back(fec_block_idx);
  }

  // Decode the blocks
  fec_decode(m_block_size,
	     block_ptrs.data(),
	     n_blocks,
	     fec_block_ptrs.data(),
	     fec_block_idxs.data(),
	     erased_block_idxs.data(),
	     erased_block_idxs.size());

  // Send the remainder of blocks that have a reasonable length.
  for (size_t i = erased_block_idxs[0]; i < n_blocks; ++i) {
    uint16_t length = blocks[i]->data_length();
    if (length <= m_block_size) {
      m_out_blocks.push(blocks[i]);
    } else {
      ++m_stats.dropped_blocks;
    }
  }
}

// Retrieve the next data/fec block
std::shared_ptr<FECBlock> FECDecoder::get_block() {
  if (m_out_blocks.empty()) {
    return std::shared_ptr<FECBlock>();
  }
  std::shared_ptr<FECBlock> ret = m_out_blocks.front();
  m_out_blocks.pop();
  return ret;
}

FECDecoderStats operator-(const FECDecoderStats& s1, const FECDecoderStats &s2) {
  FECDecoderStats ret;
  ret.total_blocks = s1.total_blocks - s2.total_blocks;
  ret.total_packets = s1.total_packets - s2.total_packets;
  ret.dropped_blocks = s1.dropped_blocks - s2.dropped_blocks;
  ret.dropped_packets = s1.dropped_packets - s2.dropped_packets;
  ret.lost_sync = s1.lost_sync - s2.lost_sync;
  ret.bytes = s1.bytes - s2.bytes;
  return ret;
}

FECDecoderStats operator+(const FECDecoderStats& s1, const FECDecoderStats &s2) {
  FECDecoderStats ret;
  ret.total_blocks = s1.total_blocks + s2.total_blocks;
  ret.total_packets = s1.total_packets + s2.total_packets;
  ret.dropped_blocks = s1.dropped_blocks + s2.dropped_blocks;
  ret.dropped_packets = s1.dropped_packets + s2.dropped_packets;
  ret.lost_sync = s1.lost_sync + s2.lost_sync;
  ret.bytes = s1.bytes + s2.bytes;
  return ret;
}


/*******************************************************************************
 * FECBufferEncoder
 ******************************************************************************/

std::vector<std::shared_ptr<FECBlock> >
FECBufferEncoder::encode_buffer(const std::vector<uint8_t> &buf) {
  size_t len = buf.size();
  std::vector<std::shared_ptr<FECBlock> > ret;

  // Divide the buffer into blocks as close to the maximum block size as possible
  uint32_t block_size = m_max_block_size;
  uint32_t nblocks = len / block_size;

  // We can only encode a maximum of 255 blocks
  if (nblocks > 255) {
    return ret;
  }
  if (nblocks == 0) {
    block_size = len;
    nblocks = 1;
  } else if ((len % block_size) != 0) {
    nblocks = len / block_size;
    block_size = len / nblocks;
    if ((nblocks * block_size) < len) {
      ++nblocks;
    }
  }

  // Create the encoder
  uint8_t nfecblocks = static_cast<uint8_t>(std::ceil(nblocks * m_fec_ratio));
  FECEncoder enc(nblocks, nfecblocks, block_size, m_seq_num++);
  if (m_seq_num == 0) {
    ++m_seq_num;
  }

  // Encode all the blocks
  uint32_t count = 0;
  for (uint32_t b = 0; b < nblocks; ++b) {
    uint32_t start = b * block_size;
    uint32_t end = std::min(start + block_size, static_cast<uint32_t>(len));
    uint32_t length = end - start;
    std::shared_ptr<FECBlock> blk = enc.get_next_block(length);
    std::copy(buf.begin() + start, buf.begin() + end, blk->data());
    enc.add_block(blk);
    count += length;
  }
  LOG_DEBUG << "Encoded " << count << " bytes and " << nblocks << " blocks";

  // Pull all the blocks out of the encoder
  for (std::shared_ptr<FECBlock> blk = enc.get_block(); blk; blk = enc.get_block()) {
    ret.push_back(blk);
  }

  return ret;
}

std::pair<uint32_t, double> FECBufferEncoder::test(uint32_t iterations) {
  uint32_t min_buffer_size = 10;
  uint32_t max_buffer_size = m_max_block_size * 255;

  uint32_t failed = 0;
  size_t bytes = 0;
  double start_time = cur_time();
  for (uint32_t i = 0; i < iterations; ++i) {

    // Create a random buffer of data
    uint32_t buf_size = min_buffer_size + rand() % (max_buffer_size - min_buffer_size);
    bytes += buf_size;
    std::vector<uint8_t> buf(buf_size);
    for (uint32_t j = 0; j < buf_size; ++j) {
      buf[j] = rand() % 255;
    }
    LOG_DEBUG << "Iteration: " << i << "  buffer size: " << buf_size;

    // Encode it
    std::vector<std::shared_ptr<FECBlock> > blks = encode_buffer(buf);
    LOG_DEBUG << blks.size() << " blocks created";

    // Decode it
    FECDecoder dec;
    std::vector<uint8_t> obuf;
    uint32_t dec_count = 0;
    for (std::shared_ptr<FECBlock> blk : blks) {
      dec.add_block(blk->pkt_data(), blk->pkt_length());
    }
    for (std::shared_ptr<FECBlock> sblk = dec.get_block(); sblk; sblk = dec.get_block()) {
      std::copy(sblk->data(), sblk->data() + sblk->data_length(),
		std::back_inserter(obuf));
      ++dec_count;
    }
    LOG_DEBUG << "Decoded " << dec_count << " blocks";

    // Compare
    if (obuf.size() != buf.size()) {
      LOG_ERROR << "Buffers are different sizes: " << obuf.size() << " != " << buf.size();
      ++failed;
    } else {
      for (size_t j = 0; j < buf.size(); ++j) {
	if (obuf[j] != buf[j]) {
	  LOG_ERROR << "Buffers differ at location " << j << ": " << obuf[j] << " != " << buf[j];
	  ++failed;
	}
      }
    }
  }

  return std::make_pair(iterations - failed,
			8e-6 * static_cast<double>(bytes) / (cur_time() - start_time));
}
