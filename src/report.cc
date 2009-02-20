/*
 * Copyright (c) 2003-2009, John Wiegley.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * - Neither the name of New Artisans LLC nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "report.h"
#include "iterators.h"
#include "filters.h"
#include "chain.h"
#include "output.h"
#include "precmd.h"
#include "emacs.h"
#include "derive.h"

namespace ledger {

void report_t::xacts_report(xact_handler_ptr handler)
{
  journal_xacts_iterator walker(*session.journal.get());
  pass_down_xacts(chain_xact_handlers(*this, handler), walker);
  session.clean_xacts();
}

void report_t::entry_report(xact_handler_ptr handler, entry_t& entry)
{
  entry_xacts_iterator walker(entry);
  pass_down_xacts(chain_xact_handlers(*this, handler), walker);
  session.clean_xacts(entry);
}

void report_t::sum_all_accounts()
{
  expr_t& expr(HANDLER(amount_).expr);
  expr.set_context(this);

  journal_xacts_iterator walker(*session.journal.get());
  pass_down_xacts(chain_xact_handlers
		  (*this, xact_handler_ptr(new set_account_value(expr)), true),
		  walker);

  expr.mark_uncompiled();	// recompile, throw away xact_t bindings
  session.master->calculate_sums(expr);
}

void report_t::accounts_report(acct_handler_ptr handler)
{
  sum_all_accounts();

  scoped_ptr<accounts_iterator> iter;

  if (! HANDLED(sort_))
    iter.reset(new basic_accounts_iterator(*session.master));
  else
    iter.reset(new sorted_accounts_iterator(HANDLER(sort_).str(),
					    HANDLED(flat), *session.master.get()));

  if (HANDLED(display_))
    pass_down_accounts(handler, *iter.get(),
		       item_predicate(HANDLER(display_).str(), what_to_keep()),
		       *this);
  else
    pass_down_accounts(handler, *iter.get());

  session.clean_xacts();
  session.clean_accounts();
}

void report_t::commodities_report(xact_handler_ptr handler)
{
  xacts_commodities_iterator walker(*session.journal.get());
  pass_down_xacts(chain_xact_handlers(*this, handler), walker);
  session.clean_xacts();
}

value_t report_t::fn_amount_expr(call_scope_t& scope)
{
  return HANDLER(amount_).expr.calc(scope);
}

value_t report_t::fn_total_expr(call_scope_t& scope)
{
  return HANDLER(total_).expr.calc(scope);
}

value_t report_t::fn_display_amount(call_scope_t& scope)
{
  return HANDLER(display_amount_).expr.calc(scope);
}

value_t report_t::fn_display_total(call_scope_t& scope)
{
  return HANDLER(display_total_).expr.calc(scope);
}

value_t report_t::fn_market_value(call_scope_t& args)
{
  var_t<datetime_t> date(args, 1);
  var_t<string>	    in_terms_of(args, 2);

  commodity_t * commodity = NULL;
  if (in_terms_of)
    commodity = amount_t::current_pool->find_or_create(*in_terms_of);

  DEBUG("report.market", "getting market value of: " << args[0]);

  value_t result =
    args[0].value(date ? optional<datetime_t>(*date) : optional<datetime_t>(),
		  commodity ? optional<commodity_t&>(*commodity) :
		  optional<commodity_t&>());

  DEBUG("report.market", "result is: " << result);
  return result;
}

value_t report_t::fn_strip(call_scope_t& args)
{
  return args[0].strip_annotations(what_to_keep());
}

value_t report_t::fn_quantity(call_scope_t& args)
{
  return args[0].to_amount().number();
}

value_t report_t::fn_truncate(call_scope_t& args)
{
  var_t<long> width(args, 1);
  var_t<long> account_abbrev(args, 2);

  return string_value(format_t::truncate(args[0].as_string(), *width,
					 account_abbrev ? *account_abbrev : -1));
}

value_t report_t::fn_print(call_scope_t& args)
{
  var_t<long> first_width(args, 1);
  var_t<long> latter_width(args, 2);
  var_t<string> date_format(args, 3);

  std::ostringstream out;

  args[0].strip_annotations(what_to_keep())
    .print(out, *first_width, latter_width ? *latter_width : -1,
	   date_format ? *date_format :
	   (HANDLED(date_format_) ?
	    HANDLER(date_format_).str() : optional<string>()));

  return string_value(out.str());
}

value_t report_t::fn_quoted(call_scope_t& args)
{
  std::ostringstream out;

  out << '"';
  foreach (const char ch, args[0].to_string()) {
    if (ch == '"')
      out << "\\\"";
    else
      out << ch;
  }
  out << '"';

  return string_value(out.str());
}

value_t report_t::fn_join(call_scope_t& args)
{
  std::ostringstream out;

  foreach (const char ch, args[0].to_string())
    if (ch != '\n')
      out << ch;

  return string_value(out.str());
}

value_t report_t::fn_format_date(call_scope_t& args)
{
  return string_value(format_date(args[0].to_date(), args[1].to_string()));
}

namespace {
  template <class Type        = xact_t,
	    class handler_ptr = xact_handler_ptr,
	    void (report_t::*report_method)(handler_ptr) =
	      &report_t::xacts_report>
  class reporter
  {
    shared_ptr<item_handler<Type> > handler;

    report_t& report;

  public:
    reporter(item_handler<Type> * _handler, report_t& _report)
      : handler(_handler), report(_report) {}

    value_t operator()(call_scope_t& args)
    {
      if (args.size() > 0) {
	report.HANDLER(limit_).on
	  (args_to_predicate_expr(args.value().as_sequence().begin(),
				  args.value().as_sequence().end()));
	DEBUG("report.predicate",
	      "Predicate = " << report.HANDLER(limit_).str());
      }

      (report.*report_method)(handler_ptr(handler));

      return true;
    }
  };
}

option_t<report_t> * report_t::lookup_option(const char * p)
{
  switch (*p) {
  case '%':
    OPT_CH(percentage);
    break;
  case 'A':
    OPT_CH(average);
    break;
  case 'B':
    OPT_CH(basis);
    break;
  case 'C':
    OPT_CH(cleared);
    break;
  case 'D':
    OPT_CH(deviation);
    break;
  case 'E':
    OPT_CH(empty);
    break;
  case 'F':
    OPT_CH(format_);
    break;
  case 'G':
    OPT_CH(gain);
    break;
  case 'I':
    OPT_CH(price);
    break;
  case 'J':
    OPT_CH(total_data);
    break;
  case 'L':
    OPT_CH(actual);
    break;
  case 'M':
    OPT_CH(monthly);
    break;
  case 'O':
    OPT_CH(quantity);
    break;
  case 'P':
    OPT_CH(by_payee);
    break;
  case 'R':
    OPT_CH(real);
    break;
  case 'S':
    OPT_CH(sort_);
    break;
  case 'T':
    OPT_CH(total_);
    break;
  case 'U':
    OPT_CH(uncleared);
    break;
  case 'V':
    OPT_CH(market);
    break;
  case 'W':
    OPT_CH(weekly);
    break;
  case 'Y':
    OPT_CH(yearly);
    break;
  case 'Z':
    OPT_CH(price_exp_);
    break;
  case 'a':
    OPT(abbrev_len_);
    else OPT(account_);
    else OPT(actual);
    else OPT(add_budget);
    else OPT(amount_);
    else OPT(amount_data);
    else OPT(anon);
    else OPT(ansi);
    else OPT(ansi_invert);
    else OPT(average);
    else OPT(account_width_);
    else OPT(amount_width_);
    break;
  case 'b':
    OPT(balance_format_);
    else OPT(base);
    else OPT_ALT(basis, cost);
    else OPT_(begin_);
    else OPT(budget);
    else OPT(by_payee);
    break;
  case 'c':
    OPT(cache_);
    else OPT(csv_format_);
    else OPT(cleared);
    else OPT(code_as_payee);
    else OPT_ALT(comm_as_payee, commodity_as_payee);
    else OPT(code_as_account);
    else OPT_ALT(comm_as_account, commodity_as_account);
    else OPT(collapse);
    else OPT(collapse_if_zero);
    else OPT(columns_);
    else OPT_(current);
    break;
  case 'd':
    OPT(daily);
    else OPT(date_format_);
    else OPT(deviation);
    else OPT_(display_);
    else OPT(display_amount_);
    else OPT(display_total_);
    else OPT(dow);
    else OPT(date_width_);
    break;
  case 'e':
    OPT(effective);
    else OPT(empty);
    else OPT_(end_);
    else OPT(equity);
    break;
  case 'f':
    OPT(flat);
    else OPT(forecast_);
    else OPT(format_);
    else OPT_ALT(head_, first_);
    break;
  case 'g':
    OPT_CH(performance);
    else OPT(gain);
    break;
  case 'h':
    OPT(head_);
    break;
  case 'i':
    OPT(invert);
    break;
  case 'j':
    OPT_CH(amount_data);
    break;
  case 'l':
    OPT_(limit_);
    else OPT(lot_dates);
    else OPT(lot_prices);
    else OPT(lot_tags);
    else OPT(lots);
    else OPT_ALT(tail_, last_);
    break;
  case 'm':
    OPT(market);
    else OPT(monthly);
    break;
  case 'n':
    OPT_CH(collapse);
    else OPT(no_total);
    break;
  case 'o':
    OPT(only_);
    else OPT_(output_);
    break;
  case 'p':
    OPT(pager_);
    else OPT(payee_as_account);
    else OPT(pending);
    else OPT(percentage);
    else OPT(performance);
    else OPT_(period_);
    else OPT(period_sort_);
    else OPT(plot_amount_format_);
    else OPT(plot_total_format_);
    else OPT(price);
    else OPT(price_exp_);
    else OPT(prices_format_);
    else OPT(pricesdb_format_);
    else OPT(print_format_);
    else OPT(payee_width_);
    break;
  case 'q':
    OPT(quantity);
    else OPT(quarterly);
    break;
  case 'r':
    OPT(real);
    else OPT(register_format_);
    else OPT_(related);
    else OPT(related_all);
    else OPT(revalued);
    else OPT(revalued_only);
    break;
  case 's':
    OPT(set_account_);
    else OPT(set_payee_);
    else OPT(set_price_);
    else OPT(sort_);
    else OPT(sort_all_);
    else OPT(sort_entries_);
    else OPT_(subtotal);
    break;
  case 't':
    OPT_CH(amount_);
    else OPT(tail_);
    else OPT(total_);
    else OPT(total_data);
    else OPT(totals);
    else OPT(truncate_);
    else OPT(total_width_);
    break;
  case 'u':
    OPT(unbudgeted);
    else OPT(uncleared);
    break;
  case 'w':
    OPT(weekly);
    else OPT_(wide);
    break;
  case 'x':
    OPT_CH(comm_as_payee);
    break;
  case 'y':
    OPT_CH(date_format_);
    else OPT(yearly);
    break;
  }
  return NULL;
}

expr_t::ptr_op_t report_t::lookup(const string& name)
{
  if (expr_t::ptr_op_t def = session.lookup(name))
    return def;

  const char * p = name.c_str();
  switch (*p) {
  case 'a':
    if (is_eq(p, "amount_expr"))
      return MAKE_FUNCTOR(report_t::fn_amount_expr);
    break;

  case 'c':
    if (WANT_CMD()) { const char * q = p + CMD_PREFIX_LEN;
      switch (*q) {
      case 'b':
	if (*(q + 1) == '\0' || is_eq(q, "bal") || is_eq(q, "balance"))
	  return expr_t::op_t::wrap_functor
	    (reporter<account_t, acct_handler_ptr, &report_t::accounts_report>
	     (new format_accounts(*this, report_format(HANDLER(balance_format_))),
	      *this));
	break;

      case 'c':
	if (is_eq(q, "csv"))
	  return WRAP_FUNCTOR
	    (reporter<>
	     (new format_xacts(*this, report_format(HANDLER(csv_format_))),
	      *this));
	break;

      case 'e':
	if (is_eq(q, "equity"))
	  return WRAP_FUNCTOR
	    (reporter<>
	     (new format_xacts(*this, report_format(HANDLER(print_format_))),
	      *this));
	else if (is_eq(q, "entry"))
	  return WRAP_FUNCTOR(entry_command);
	else if (is_eq(q, "emacs"))
	  return WRAP_FUNCTOR
	    (reporter<>(new format_emacs_xacts(output_stream), *this));
	break;

      case 'p':
	if (*(q + 1) == '\0' || is_eq(q, "print"))
	  return WRAP_FUNCTOR
	    (reporter<>
	     (new format_xacts(*this, report_format(HANDLER(print_format_))),
	      *this));
	else if (is_eq(q, "prices"))
	  return expr_t::op_t::wrap_functor
	    (reporter<xact_t, xact_handler_ptr, &report_t::commodities_report>
	     (new format_xacts(*this, report_format(HANDLER(prices_format_))),
	      *this));
	else if (is_eq(q, "pricesdb"))
	  return expr_t::op_t::wrap_functor
	    (reporter<xact_t, xact_handler_ptr, &report_t::commodities_report>
	     (new format_xacts(*this, report_format(HANDLER(pricesdb_format_))),
	      *this));
	break;

      case 'r':
	if (*(q + 1) == '\0' || is_eq(q, "reg") || is_eq(q, "register"))
	  return WRAP_FUNCTOR
	    (reporter<>
	     (new format_xacts(*this, report_format(HANDLER(register_format_))),
	      *this));
	else if (is_eq(q, "reload"))
	  return MAKE_FUNCTOR(report_t::reload_command);
	break;

      case 's':
	if (is_eq(q, "stats") || is_eq(q, "stat"))
	  return WRAP_FUNCTOR(reporter<>(new gather_statistics(*this), *this));
	break;
      }
    }
    break;

  case 'd':
    if (is_eq(p, "display_amount"))
      return MAKE_FUNCTOR(report_t::fn_display_amount);
    else if (is_eq(p, "display_total"))
      return MAKE_FUNCTOR(report_t::fn_display_total);
    break;

  case 'f':
    if (is_eq(p, "format_date"))
      return MAKE_FUNCTOR(report_t::fn_format_date);
    break;

  case 'j':
    if (is_eq(p, "join"))
      return MAKE_FUNCTOR(report_t::fn_join);
    break;

  case 'm':
    if (is_eq(p, "market"))
      return MAKE_FUNCTOR(report_t::fn_market_value);
    break;

  case 'o':
    if (WANT_OPT()) { const char * q = p + OPT_PREFIX_LEN;
      if (option_t<report_t> * handler = lookup_option(q))
	return MAKE_OPT_HANDLER(report_t, handler);
    }
    else if (is_eq(p, "options")) {
      return MAKE_FUNCTOR(report_t::fn_options);
    }
    break;

  case 'p':
    if (WANT_PRECMD()) { const char * q = p + PRECMD_PREFIX_LEN;
      switch (*q) {
      case 'a':
	if (is_eq(q, "args"))
	  return WRAP_FUNCTOR(args_command);
	break;
      case 'e':
	if (is_eq(q, "eval"))
	  return WRAP_FUNCTOR(eval_command);
	break;
      case 'f':
	if (is_eq(q, "format"))
	  return WRAP_FUNCTOR(format_command);
	break;
      case 'p':
	if (is_eq(q, "parse"))
	  return WRAP_FUNCTOR(parse_command);
	else if (is_eq(q, "period"))
	  return WRAP_FUNCTOR(period_command);
	break;
      case 't':
	if (is_eq(q, "template"))
	  return WRAP_FUNCTOR(template_command);
	break;
      }
    }
    else if (is_eq(p, "print"))
      return MAKE_FUNCTOR(report_t::fn_print);
    break;

  case 'q':
    if (is_eq(p, "quoted"))
      return MAKE_FUNCTOR(report_t::fn_quoted);
    else if (is_eq(p, "quantity"))
      return MAKE_FUNCTOR(report_t::fn_quantity);
    break;

  case 's':
    if (is_eq(p, "strip"))
      return MAKE_FUNCTOR(report_t::fn_strip);
    break;

  case 't':
    if (is_eq(p, "truncate"))
      return MAKE_FUNCTOR(report_t::fn_truncate);
    else if (is_eq(p, "total_expr"))
      return MAKE_FUNCTOR(report_t::fn_total_expr);
    break;

  case 'x':
    if (is_eq(p, "xact"))
      return MAKE_FUNCTOR(report_t::fn_false);
    break;
  }

  // Check if they are trying to access an option's setting or value.
  if (option_t<report_t> * handler = lookup_option(p))
    return MAKE_OPT_FUNCTOR(report_t, handler);

  return NULL;
}

} // namespace ledger
