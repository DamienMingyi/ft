// Copyright [2020] <Copyright Kevin, kevin.lau.gd@gmail.com>

#include "trading_server/order_management/portfolio.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "trading_server/datastruct/constants.h"
#include "utils/contract_table.h"

namespace ft {

Portfolio::Portfolio() {}

void Portfolio::Init(uint32_t max_tid, bool sync_to_redis, uint64_t account) {
  if (sync_to_redis) {
    redis_ = std::make_unique<RedisPositionSetter>();
    redis_->set_account(account);
    redis_->Clear();
  }

  positions_.resize(max_tid + 1);
  uint32_t ticker_id = 0;
  for (auto& pos : positions_) pos.ticker_id = ticker_id++;
}

void Portfolio::set_position(const Position& pos) {
  positions_[pos.ticker_id] = pos;

  if (redis_) {
    auto contract = ContractTable::get_by_index(pos.ticker_id);
    assert(contract);
    redis_->set(contract->ticker, pos);
  }
}

void Portfolio::UpdatePending(uint32_t ticker_id, Direction direction, Offset offset, int changed) {
  if (changed == 0) return;

  if (direction == Direction::kBuy || direction == Direction::kSell) {
    UpdateBuyOrSellPending(ticker_id, direction, offset, changed);
  } else if (direction == Direction::kPurchase || direction == Direction::kRedeem) {
    UpdatePurchaseOrRedeemPending(ticker_id, direction, changed);
  }
}

void Portfolio::UpdateBuyOrSellPending(uint32_t ticker_id, Direction direction, Offset offset,
                                       int changed) {
  bool is_close = IsOffsetClose(offset);
  if (is_close) direction = OppositeDirection(direction);

  auto& pos = positions_[ticker_id];
  auto& pos_detail = direction == Direction::kBuy ? pos.long_pos : pos.short_pos;
  if (is_close)
    pos_detail.close_pending += changed;
  else
    pos_detail.open_pending += changed;

  if (pos_detail.open_pending < 0) {
    pos_detail.open_pending = 0;
    // spdlog::warn("[Portfolio::UpdateBuyOrSellPending] correct
    // open_pending");
  }

  if (pos_detail.close_pending < 0) {
    pos_detail.close_pending = 0;
    // spdlog::warn("[Portfolio::UpdateBuyOrSellPending] correct
    // close_pending");
  }

  if (redis_) {
    const auto* contract = ContractTable::get_by_index(pos.ticker_id);
    assert(contract);
    redis_->set(contract->ticker, pos);
  }
}

void Portfolio::UpdatePurchaseOrRedeemPending(uint32_t ticker_id, Direction direction,
                                              int changed) {
  auto& pos = positions_[ticker_id];
  auto& pos_detail = pos.long_pos;

  if (direction == Direction::kPurchase) {
    pos_detail.open_pending += changed;
  } else {
    pos_detail.close_pending += changed;
  }

  if (pos_detail.open_pending < 0) {
    pos_detail.open_pending = 0;
    // spdlog::warn("[Portfolio::UpdatePurchaseOrRedeemPending] correct
    // open_pending");
  }

  if (pos_detail.close_pending < 0) {
    pos_detail.close_pending = 0;
    // spdlog::warn("[Portfolio::UpdatePurchaseOrRedeemPending] correct
    // close_pending");
  }

  if (redis_) {
    const auto* contract = ContractTable::get_by_index(pos.ticker_id);
    assert(contract);
    redis_->set(contract->ticker, pos);
  }
}

void Portfolio::UpdateTraded(uint32_t ticker_id, Direction direction, Offset offset, int traded,
                             double traded_price) {
  if (traded <= 0) return;

  if (direction == Direction::kBuy || direction == Direction::kSell) {
    UpdateBuyOrSell(ticker_id, direction, offset, traded, traded_price);
  } else if (direction == Direction::kPurchase || direction == Direction::kRedeem) {
    UpdatePurchaseOrRedeem(ticker_id, direction, traded);
  }
}

void Portfolio::UpdateBuyOrSell(uint32_t ticker_id, Direction direction, Offset offset, int traded,
                                double traded_price) {
  bool is_close = IsOffsetClose(offset);
  if (is_close) direction = OppositeDirection(direction);

  auto& pos = positions_[ticker_id];
  auto& pos_detail = direction == Direction::kBuy ? pos.long_pos : pos.short_pos;

  if (is_close) {
    pos_detail.close_pending -= traded;
    pos_detail.holdings -= traded;
    // 这里close_yesterday也执行这个操作是为了防止有些交易所不区分昨今仓，
    // 但用户平仓的时候却使用了close_yesterday
    if (offset == Offset::kCloseYesterday || offset == Offset::kClose)
      pos_detail.yd_holdings -= std::min(pos_detail.yd_holdings, traded);

    if (pos_detail.holdings < pos_detail.yd_holdings) {
      spdlog::warn("yd pos fixed");
      pos_detail.yd_holdings = pos_detail.holdings;
    }
  } else {
    pos_detail.open_pending -= traded;
    pos_detail.holdings += traded;
  }

  // TODO(kevin): 这里可能出问题
  // 如果abort可能是trade在position之前到达，正常使用不可能出现
  // 如果close_pending小于0，也有可能是之前启动时的挂单成交了，
  // 这次重启时未重启获取挂单数量导致的
  assert(pos_detail.holdings >= 0);

  if (pos_detail.open_pending < 0) {
    pos_detail.open_pending = 0;
    // spdlog::warn("[Portfolio::UpdateTraded] correct open_pending");
  }

  if (pos_detail.close_pending < 0) {
    pos_detail.close_pending = 0;
    // spdlog::warn("[Portfolio::UpdateTraded] correct close_pending");
  }

  const auto* contract = ContractTable::get_by_index(ticker_id);
  if (!contract) {
    spdlog::error("[Position::UpdateBuyOrSell] Contract not found");
    return;
  }
  assert(contract->size > 0);

  // 如果是开仓则计算当前持仓的成本价
  if (IsOffsetOpen(offset) && pos_detail.holdings > 0) {
    double cost = contract->size * (pos_detail.holdings - traded) * pos_detail.cost_price +
                  contract->size * traded * traded_price;
    pos_detail.cost_price = cost / (pos_detail.holdings * contract->size);
  }

  if (pos_detail.holdings == 0) {
    pos_detail.cost_price = 0;
  }

  if (redis_) redis_->set(contract->ticker, pos);
}

void Portfolio::UpdatePurchaseOrRedeem(uint32_t ticker_id, Direction direction, int traded) {
  auto& pos = positions_[ticker_id];
  auto& pos_detail = pos.long_pos;

  if (direction == Direction::kPurchase) {
    pos_detail.open_pending -= traded;
    pos_detail.holdings += traded;
    pos_detail.yd_holdings += traded;
  } else {
    pos_detail.close_pending -= traded;

    int td_pos = pos_detail.holdings - pos_detail.yd_holdings;
    pos_detail.holdings -= traded;
    pos_detail.yd_holdings -= std::max(traded - td_pos, 0);

    if (pos_detail.holdings == 0) {
      pos_detail.float_pnl = 0;
      pos_detail.cost_price = 0;
    }
  }

  if (redis_) {
    const auto* contract = ContractTable::get_by_index(ticker_id);
    if (!contract) {
      spdlog::error("[Position::UpdatePurchaseOrRedeem] Contract not found");
      return;
    }
    redis_->set(contract->ticker, pos);
  }
}

void Portfolio::UpdateComponentStock(uint32_t ticker_id, int traded, bool acquire) {
  auto& pos = positions_[ticker_id];
  auto& pos_detail = pos.long_pos;

  if (acquire) {
    pos_detail.holdings += traded;
    pos_detail.yd_holdings += traded;
  } else {
    int td_pos = pos_detail.holdings - pos_detail.yd_holdings;
    pos_detail.holdings -= traded;
    pos_detail.yd_holdings -= std::max(traded - td_pos, 0);
  }

  if (redis_) {
    const auto* contract = ContractTable::get_by_index(ticker_id);
    if (!contract) {
      spdlog::error("[Position::UpdatePurchaseOrRedeem] Contract not found");
      return;
    }
    redis_->set(contract->ticker, pos);
  }
}

void Portfolio::UpdateFloatPnl(uint32_t ticker_id, double last_price) {
  auto& pos = positions_[ticker_id];
  if (pos.long_pos.holdings > 0 || pos.short_pos.holdings > 0) {
    const auto* contract = ContractTable::get_by_index(ticker_id);
    if (!contract || contract->size <= 0) return;

    auto& lp = pos.long_pos;
    auto& sp = pos.short_pos;

    if (lp.holdings > 0) lp.float_pnl = lp.holdings * contract->size * (last_price - lp.cost_price);

    if (sp.holdings > 0) sp.float_pnl = sp.holdings * contract->size * (sp.cost_price - last_price);

    if (redis_) {
      if (lp.holdings > 0 || sp.holdings > 0) {
        redis_->set(contract->ticker, pos);
      }
    }
  }
}

void Portfolio::UpdateOnQueryTrade(uint32_t ticker_id, Direction direction, Offset offset,
                                   int closed_volume) {
  // auto* pos = find(ticker_id);
  // if (!pos) return;

  // const auto* contract = ContractTable::get_by_index(ticker_id);
  // if (!contract) return;

  // redis_.set(proto_.pos_key(contract->ticker), pos, sizeof(*pos));
}

}  // namespace ft
