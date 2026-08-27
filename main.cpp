#include <iostream>
#include <list>
#include <map>
#include <numeric>
#include <utility>

enum class OrderType {
    GoodTillCancel,
    FillAndKill
};

enum class Side {
    Buy,
    Sell,
};

using Price = std::int32_t;
using Quantity = std::uint32_t;
using OrderId = std::uint32_t;

struct LevelInfo {
    Price price_;
    Quantity quantity_;
};

using LevelInfos = std::vector<LevelInfo>;

class OrderBookLevelInfos {
public:
    OrderBookLevelInfos(LevelInfos bids, LevelInfos asks) : bids_(std::move(bids)), asks_(std::move(asks)) {
    };

    [[nodiscard]] const LevelInfos &GetBids() const { return bids_; }
    [[nodiscard]] const LevelInfos &GetAsks() const { return asks_; }

private:
    LevelInfos bids_;
    LevelInfos asks_;
};

class Order {
public:
    Order(const OrderType orderType,
          const OrderId orderId,
          const Side side,
          const Price price,
          const Quantity quantity)
        : orderType_(orderType),
          orderId_(orderId),
          side_(side),
          price_(price),
          initialQuantity_(quantity),
          remainingQuantity_(quantity) {
    };

    [[nodiscard]] OrderId getOrderId() const { return orderId_; };
    [[nodiscard]] Side getSide() const { return side_; };
    [[nodiscard]] Price getPrice() const { return price_; };
    [[nodiscard]] OrderType getOrderType() const { return orderType_; };
    [[nodiscard]] Quantity getInitialQuantity() const { return initialQuantity_; };
    [[nodiscard]] Quantity getRemainingQuantity() const { return remainingQuantity_; };
    [[nodiscard]] Quantity getFilledQuantity() const { return initialQuantity_ - remainingQuantity_; };

    [[nodiscard]] bool isFilled() const { return getRemainingQuantity() == 0; };

    void Fill(Quantity quantity) {
        if (quantity > getRemainingQuantity()) {
            throw std::logic_error(std::format("Order ({}) cannot be filled for more than its remaining quantity",
                                               getOrderId()));
        }

        remainingQuantity_ -= quantity;
    }

private:
    OrderType orderType_;
    OrderId orderId_;
    Side side_;
    Price price_;
    Quantity initialQuantity_{};
    Quantity remainingQuantity_{};
};

using OrderPointer = std::shared_ptr<Order>;
using OrderPointers = std::list<OrderPointer>;

class OrderModify {
public:
    OrderModify(const OrderId orderId, const Side side, const Price price, const Quantity quantity) : orderId_(orderId),
        side_(side), price_(price), quantity_(quantity) {
    }

    [[nodiscard]] OrderId getOrderId() const { return orderId_; };
    [[nodiscard]] Side getSide() const { return side_; };
    [[nodiscard]] Price getPrice() const { return price_; };
    [[nodiscard]] Quantity getQuantity() const { return quantity_; };

    [[nodiscard]] OrderPointer ToOrderPointer(OrderType type) const {
        return std::make_shared<Order>(type, getOrderId(), getSide(), price_, quantity_);
    }

private:
    OrderId orderId_;
    Side side_;
    Price price_;
    Quantity quantity_;
};

struct TradeInfo {
    OrderId orderId_;
    Price price_;
    Quantity quantity_;
};

class Trade {
public:
    Trade(const TradeInfo &bidTrade, const TradeInfo &askTrade) : bidTrade_(bidTrade), askTrade_(askTrade) {
    }

    [[nodiscard]] const TradeInfo &GetBidTrade() const { return bidTrade_; };
    [[nodiscard]] const TradeInfo &GetAskTrade() const { return askTrade_; };

private:
    TradeInfo bidTrade_;
    TradeInfo askTrade_;
};


using Trades = std::vector<Trade>;

class OrderBook {
public:
    Trades AddOrder(const OrderPointer &order) {
        if (orders_.contains(order->getOrderId())) {
            return {};
        }

        if (order->getOrderType() == OrderType::FillAndKill && !CanMatch(order->getSide(), order->getPrice())) {
            return {};
        }

        OrderPointers::iterator iterator;

        if (order->getSide() == Side::Buy) {
            auto &orders = bids_[order->getPrice()];
            orders.push_back(order);
            iterator = std::next(orders.begin(), orders.size() - 1);
        } else {
            auto &orders = asks_[order->getPrice()];
            orders.push_back(order);
            iterator = std::next(orders.begin(), orders.size() - 1);
        }

        orders_.insert({
            order->getOrderId(),
            OrderEntry{order, iterator}
        });

        return MatchOrders();
    }

    void CancelOrder(const OrderId orderId) {
        if (!orders_.contains(orderId)) {
            return;
        }

        const auto &[order, orderItr] = orders_.at(orderId);

        if (order->getSide() == Side::Sell) {
            const auto price = order->getPrice();
            auto &orders = asks_.at(price);
            orders.erase(orderItr);

            if (orders.empty()) {
                asks_.erase(price);
            }
        } else {
            const auto price = order->getPrice();
            auto &orders = bids_.at(price);
            orders.erase(orderItr);

            if (orders.empty()) {
                bids_.erase(price);
            }
        }

        orders_.erase(orderId);
    }

    Trades MatchOrder(OrderModify &order) {
        if (!orders_.contains(order.getOrderId())) {
            return {};
        }

        const auto &[existingOrder, _] = orders_.at(order.getOrderId());

        CancelOrder(order.getOrderId());
        return AddOrder(order.ToOrderPointer(existingOrder->getOrderType()));
    }

    [[nodiscard]] std::size_t Size() const { return orders_.size(); }

    [[nodiscard]] OrderBookLevelInfos GetOrderInfos() const {
        LevelInfos bidInfos, askInfos;
        bidInfos.reserve(orders_.size());
        askInfos.reserve(orders_.size());

        auto CreateLevelInfos = [](Price price, const OrderPointers &orders) {
            return LevelInfo{
                price,
                std::accumulate(orders.begin(), orders.end(), static_cast<Quantity>(0),
                                [](std::size_t runningSum, const OrderPointer &order) {
                                    return runningSum + order->getRemainingQuantity();
                                })
            };
        };

        for (const auto &[price, orders]: bids_) {
            bidInfos.push_back(CreateLevelInfos(price, orders));
        }

        for (const auto &[price, orders]: asks_) {
            askInfos.push_back(CreateLevelInfos(price, orders));
        }

        return OrderBookLevelInfos{bidInfos, askInfos};
    }

private:
    struct OrderEntry {
        OrderPointer order_{nullptr};
        OrderPointers::iterator location_;
    };

    std::map<Price, OrderPointers, std::greater<> > bids_;
    std::map<Price, OrderPointers, std::less<> > asks_;

    std::unordered_map<OrderId, OrderEntry> orders_;

    [[nodiscard]] bool CanMatch(const Side side, const Price price) const {
        if (side == Side::Buy) {
            if (asks_.empty()) {
                return false;
            }

            const auto &[bestAsk, _] = *asks_.begin();

            return price >= bestAsk;
        } else {
            if (bids_.empty()) {
                return false;
            }

            const auto &[bestBid, _] = *bids_.begin();
            return price <= bestBid;
        }
    }

    Trades MatchOrders() {
        Trades trades;
        trades.reserve(orders_.size());

        while (true) {
            if (bids_.empty() || asks_.empty()) {
                break;
            }

            auto &[bidPrice, bids] = *bids_.begin();
            auto &[askPrice, asks] = *asks_.begin();

            if (bidPrice < askPrice) {
                break;
            }

            while (!bids.empty() && !asks.empty()) {
                const auto bid = bids.front();
                const auto ask = asks.front();

                Quantity quantity = std::min(bid->getRemainingQuantity(), ask->getRemainingQuantity());
                bid->Fill(quantity);
                ask->Fill(quantity);

                if (bid->isFilled()) {
                    bids.pop_front();
                    orders_.erase(bid->getOrderId());
                }

                if (ask->isFilled()) {
                    asks.pop_front();
                    orders_.erase(ask->getOrderId());
                }

                if (bids.empty()) {
                    bids_.erase(bidPrice);
                }

                if (asks.empty()) {
                    asks_.erase(askPrice);
                }

                trades.emplace_back(
                    TradeInfo{bid->getOrderId(), bid->getPrice(), quantity},
                    TradeInfo{ask->getOrderId(), ask->getPrice(), quantity}
                );
            }
        }

        if (!bids_.empty()) {
            auto &[_, bids] = *bids_.begin();
            auto &order = bids.front();
            if (order->getOrderType() == OrderType::FillAndKill) {
                CancelOrder(order->getOrderId());
            }
        }

        if (!asks_.empty()) {
            auto &[_, asks] = *asks_.begin();
            auto &order = asks.front();
            if (order->getOrderType() == OrderType::FillAndKill) {
                CancelOrder(order->getOrderId());
            }
        }

        return trades;
    }
};

int main() {
    OrderBook orderBook;
    OrderId orderId = 1;
    orderBook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, orderId, Side::Buy, 100, 10));

    std::cout << orderBook.Size() << std::endl;

    orderBook.CancelOrder(orderId);

    std::cout << orderBook.Size() << std::endl;

    return 0;
}
