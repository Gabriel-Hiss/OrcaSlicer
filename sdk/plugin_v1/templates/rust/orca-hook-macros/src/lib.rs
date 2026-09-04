use proc_macro::TokenStream;
use quote::quote;
use syn::{parse_macro_input, ItemFn};
fn make_wrapper(kind: &str, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as ItemFn);
    let sig = &input.sig;
    let block = &input.block;
    let ident = &input.sig.ident;
    let vis = &input.vis;
    let attrs = &input.attrs;
    let kind_str = kind;
    TokenStream::from(quote! {
        #(#attrs)* #vis #sig {
            let _kind = #kind_str;
            let _guard = std::panic::AssertUnwindSafe(|| #block);
            match std::panic::catch_unwind(_guard) {
                Ok(v) => v,
                Err(e) => { let _ = e; return; }
            }
        }
        const _: () = {};
        const _: Option<fn()> = Some(#ident as fn());
    })
}
#[proc_macro_attribute]
pub fn hook(attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as ItemFn);
    let fn_name = &input.sig.ident;
    let fn_block = &input.block;
    let fn_sig = &input.sig;
    let attr_str = attr.to_string();
    let expanded = quote! {
        #fn_sig {
            let _attr = #attr_str;
            let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| #fn_block));
            match res { Ok(v) => v, Err(_) => { return; } }
        }
        const _: () = {};
    };
    let orig = quote! { #input };
    let _ = fn_name;
    TokenStream::from(quote! { #orig #expanded })
}
#[proc_macro_attribute]
pub fn before(_attr: TokenStream, item: TokenStream) -> TokenStream { make_wrapper("before", item) }
#[proc_macro_attribute]
pub fn after(_attr: TokenStream, item: TokenStream) -> TokenStream { make_wrapper("after", item) }
#[proc_macro_attribute]
pub fn replace(_attr: TokenStream, item: TokenStream) -> TokenStream { make_wrapper("replace", item) }
#[proc_macro_attribute]
pub fn at(_attr: TokenStream, item: TokenStream) -> TokenStream { make_wrapper("at", item) }
