/** \copyright
 * Copyright (c) 2014, Balazs Racz
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are  permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * \file LocalTrackIf.hxx
 *
 * Control flow that acts as a trackInterface and sends all packets to a local
 * fd that represents the DCC mainline, such as TivaDCC.
 *
 * NOTE: This has been customized for ESP32 Command Station to split the OPS
 * and PROG ioctl handling based on the send_long_preamble header flag. This
 * is not intended for merge back to OpenMRN.
 * 
 * @author Balazs Racz
 * @date 24 Aug 2014
 */

#include "can_ioctl.h"

#include <dcc/LocalTrackIf.hxx>
#include <dcc/Packet.hxx>
#include <executor/Executor.hxx>
#include <executor/StateFlow.hxx>
#include <sys/ioctl.h>
#include <utils/Buffer.hxx>
#include <utils/Queue.hxx>

namespace dcc
{

LocalTrackIf::LocalTrackIf(Service *service, int pool_size)
    : StateFlow<Buffer<dcc::Packet>, QList<1>>(service)
    , fd_(-1)
    , pool_(sizeof(Buffer<dcc::Packet>), pool_size)
{
}

StateFlowBase::Action LocalTrackIf::entry()
{
  HASSERT(fd_ >= 0);
  auto *p = message()->data();
  int ret = ::write(fd_, p, sizeof(*p));
  if (ret < 0)
  {
    HASSERT(errno == ENOSPC);
    ::ioctl(fd_
          , p->packet_header.send_long_preamble ?
            CAN_IOC_WRITE_PROG_ACTIVE : CAN_IOC_WRITE_OPS_ACTIVE
          , this);
    return wait();
  }
  return finish();
}

} // namespace dcc